/* SPDX-License-Identifier: MIT
 *
 * polaris-extract — turn a camera JPEG into a star list for polaris-solve.
 *
 * Deliberately ours rather than astrometry.net's `simplexy`: that one pulls in
 * the GPLv3 `ctmf.c`, and keeping extraction in our own MIT code is what lets
 * the licence boundary sit at the process boundary (docs/LICENSE-AUDIT.md).
 *
 * Sized for a 45 MP full-frame frame at up to 400 mm: it decodes at a reduced
 * scale (libjpeg can do 1/2, 1/4, 1/8 during decode, which is far cheaper than
 * decoding full size and shrinking), finds blobs above a locally-estimated
 * background, and reports flux-weighted centroids in FULL-RESOLUTION pixel
 * coordinates so the caller's scale hint stays in full-res terms.
 *
 *   polaris-extract --jpeg IMG.JPG [--downsample 4] [--max-stars 200] > stars.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <jpeglib.h>
#include <sys/time.h>

typedef struct { double x, y, flux; int npix; } star_t;

static int cmp_flux(const void* a, const void* b) {
    double d = ((const star_t*)b)->flux - ((const star_t*)a)->flux;
    return (d > 0) - (d < 0);
}

static void usage(const char* me) {
    fprintf(stderr,
"usage: %s --jpeg FILE [options] > stars.txt\n"
"  --downsample N    decode at 1/N scale: 1,2,4,8 (default 4)\n"
"  --sigma K         detect above background + K*noise (default 5)\n"
"  --min-pixels N    smallest blob to keep      (default 3)\n"
"  --max-pixels N    largest blob to keep       (default 500; rejects the moon,\n"
"                    nebulosity and clipped glare)\n"
"  --max-stars N     keep the brightest N       (default 300)\n"
"  --margin PX       ignore blobs within PX of the edge, full-res (default 16)\n"
"  --y-origin O      'bottom' (FITS convention, the default, and what\n"
"                    astrometry.net's own .xyls files use) or 'top' (raw image\n"
"                    rows). Getting this wrong flips the parity and the\n"
"                    reported roll angle.\n"
"  --stats           print a summary to stderr\n"
"\n"
"Output: one star per line, \"x y flux\", brightest first, in FULL-RESOLUTION\n"
"pixel coordinates regardless of --downsample.\n", me);
}

int main(int argc, char** argv) {
    const char* fn = NULL;
    int ds = 4, minpix = 3, maxpix = 500, maxstars = 300, margin = 16, stats = 0;
    int y_bottom = 1;                 /* FITS convention by default */
    double ksigma = 5.0;
    int i;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE* f;
    unsigned char* gray = NULL;
    int W, H, fullW, fullH;
    JSAMPARRAY buf;
    double bg, noise;
    int* label = NULL;
    int* stack = NULL;
    star_t* stars = NULL;
    int nstars = 0;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (usage(argv[0]), exit(2), ""))
        if      (!strcmp(a, "--jpeg"))       fn = NEXT();
        else if (!strcmp(a, "--downsample")) ds = atoi(NEXT());
        else if (!strcmp(a, "--sigma"))      ksigma = atof(NEXT());
        else if (!strcmp(a, "--min-pixels")) minpix = atoi(NEXT());
        else if (!strcmp(a, "--max-pixels")) maxpix = atoi(NEXT());
        else if (!strcmp(a, "--max-stars"))  maxstars = atoi(NEXT());
        else if (!strcmp(a, "--margin"))     margin = atoi(NEXT());
        else if (!strcmp(a, "--y-origin"))   { const char* v = NEXT(); y_bottom = strcmp(v, "top") != 0; }
        else if (!strcmp(a, "--stats"))      stats = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 2; }
        #undef NEXT
    }
    if (!fn) { usage(argv[0]); return 2; }
    if (ds != 1 && ds != 2 && ds != 4 && ds != 8) {
        fprintf(stderr, "--downsample must be 1, 2, 4 or 8\n"); return 2;
    }

    struct timeval tv0, tv1;
    gettimeofday(&tv0, NULL);

    f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", fn); return 1; }
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    fullW = cinfo.image_width; fullH = cinfo.image_height;
    /* libjpeg gives us the reduced size almost free during the IDCT */
    cinfo.scale_num = 1; cinfo.scale_denom = ds;
    cinfo.out_color_space = JCS_GRAYSCALE;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&cinfo);
    W = cinfo.output_width; H = cinfo.output_height;
    gray = (unsigned char*)malloc((size_t)W * H);
    if (!gray) { fprintf(stderr, "out of memory for %dx%d\n", W, H); return 1; }
    buf = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, W, 1);
    while (cinfo.output_scanline < (unsigned)H) {
        int row = cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, buf, 1);
        memcpy(gray + (size_t)row * W, buf[0], W);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);

    /* Background and noise from a sparse sample: the sky dominates by area, so
     * the median is a robust background and the MAD a robust noise estimate. */
    {
        int nsamp = 0, cap = 200000;
        unsigned char* samp = (unsigned char*)malloc(cap);
        int step = (int)sqrt(((double)W * H) / (double)cap) + 1;
        int x, y;
        long hist[256];
        long acc, half;
        for (y = 0; y < H; y += step)
            for (x = 0; x < W; x += step)
                if (nsamp < cap) samp[nsamp++] = gray[(size_t)y * W + x];
        memset(hist, 0, sizeof(hist));
        for (i = 0; i < nsamp; i++) hist[samp[i]]++;
        acc = 0; half = nsamp / 2; bg = 0;
        for (i = 0; i < 256; i++) { acc += hist[i]; if (acc >= half) { bg = i; break; } }
        /* MAD -> sigma */
        memset(hist, 0, sizeof(hist));
        for (i = 0; i < nsamp; i++) hist[(int)fabs(samp[i] - bg)]++;
        acc = 0; noise = 1.0;
        for (i = 0; i < 256; i++) { acc += hist[i]; if (acc >= half) { noise = i; break; } }
        noise = noise * 1.4826;
        if (noise < 1.0) noise = 1.0;
        free(samp);
    }

    /* Connected components above threshold, iterative flood fill. */
    {
        double thresh = bg + ksigma * noise;
        int cap = 4096;
        int x, y;
        label = (int*)calloc((size_t)W * H, sizeof(int));
        stack = (int*)malloc((size_t)cap * sizeof(int));
        stars = (star_t*)malloc((size_t)(maxstars * 8 + 1024) * sizeof(star_t));
        int starcap = maxstars * 8 + 1024;
        if (!label || !stack || !stars) { fprintf(stderr, "out of memory\n"); return 1; }
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                size_t idx = (size_t)y * W + x;
                int sp = 0, npix = 0;
                double sx = 0, sy = 0, sf = 0;
                int minx, maxx, miny, maxy;
                if (label[idx] || gray[idx] <= thresh) continue;
                label[idx] = 1; stack[sp++] = (int)idx;
                minx = maxx = x; miny = maxy = y;
                while (sp > 0) {
                    int cur = stack[--sp];
                    int cx = cur % W, cy = cur / W;
                    double v = gray[cur] - bg;
                    int dx, dy;
                    if (v < 0) v = 0;
                    npix++; sx += cx * v; sy += cy * v; sf += v;
                    if (cx < minx) minx = cx; if (cx > maxx) maxx = cx;
                    if (cy < miny) miny = cy; if (cy > maxy) maxy = cy;
                    if (npix > maxpix) { /* too big: drain and drop */
                        while (sp > 0) { int c2 = stack[--sp]; label[c2] = 1; }
                        break;
                    }
                    for (dy = -1; dy <= 1; dy++) {
                        for (dx = -1; dx <= 1; dx++) {
                            int nx = cx + dx, ny = cy + dy;
                            size_t nidx;
                            if (dx == 0 && dy == 0) continue;
                            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                            nidx = (size_t)ny * W + nx;
                            if (label[nidx] || gray[nidx] <= thresh) continue;
                            label[nidx] = 1;
                            if (sp == cap) { cap *= 2; stack = (int*)realloc(stack, (size_t)cap * sizeof(int)); }
                            stack[sp++] = (int)nidx;
                        }
                    }
                }
                if (npix < minpix || npix > maxpix || sf <= 0) continue;
                /* Reject strongly elongated blobs: star trails from a drifting
                 * mount, satellites, and hot-pixel rows are not point sources. */
                {
                    double w = maxx - minx + 1, h = maxy - miny + 1;
                    double elong = (w > h) ? w / h : h / w;
                    if (elong > 3.0) continue;
                }
                if (nstars < starcap) {
                    double fx = (sx / sf) * ds, fy = (sy / sf) * ds;
                    if (fx < margin || fy < margin || fx > fullW - margin || fy > fullH - margin)
                        continue;
                    stars[nstars].x = fx; stars[nstars].y = fy;
                    stars[nstars].flux = sf; stars[nstars].npix = npix;
                    nstars++;
                }
            }
        }
    }

    qsort(stars, nstars, sizeof(star_t), cmp_flux);
    if (nstars > maxstars) nstars = maxstars;

    gettimeofday(&tv1, NULL);
    if (stats)
        fprintf(stderr,
                "[extract] %dx%d decoded at 1/%d (full %dx%d), bg=%.1f noise=%.2f "
                "thresh=%.1f stars=%d  %.3f s\n",
                W, H, ds, fullW, fullH, bg, noise, bg + ksigma * noise, nstars,
                (tv1.tv_sec - tv0.tv_sec) + (tv1.tv_usec - tv0.tv_usec) * 1e-6);

    printf("# polaris-extract: %s\n# full resolution %d x %d\n# y-origin: %s\n",
           fn, fullW, fullH, y_bottom ? "bottom (FITS)" : "top (image)");
    for (i = 0; i < nstars; i++) {
        double yy = y_bottom ? (fullH - 1 - stars[i].y) : stars[i].y;
        printf("%.3f %.3f %.1f\n", stars[i].x, yy, stars[i].flux);
    }

    free(gray); free(label); free(stack); free(stars);
    return nstars > 0 ? 0 : 3;
}
