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

typedef struct { double x, y, flux, hfr; int npix; } star_t;

static float* bgmap = NULL;      /* per-tile background */
static float* nsmap = NULL;      /* per-tile noise (MAD -> sigma) */
static int tiles_w = 0, tiles_h = 0, tile = 64;

/* bilinear interpolation between tile centres */
static double map_at(const float* m, int x, int y) {
    double fx = ((double)x - tile * 0.5) / tile;
    double fy = ((double)y - tile * 0.5) / tile;
    int x0, y0, x1, y1;
    double ax, ay;
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    x0 = (int)fx; y0 = (int)fy;
    x1 = x0 + 1;  y1 = y0 + 1;
    if (x1 > tiles_w - 1) x1 = tiles_w - 1;
    if (y1 > tiles_h - 1) y1 = tiles_h - 1;
    if (x0 > tiles_w - 1) x0 = tiles_w - 1;
    if (y0 > tiles_h - 1) y0 = tiles_h - 1;
    ax = fx - (int)fx; ay = fy - (int)fy;
    return (1 - ax) * (1 - ay) * m[y0 * tiles_w + x0] + ax * (1 - ay) * m[y0 * tiles_w + x1]
         + (1 - ax) * ay * m[y1 * tiles_w + x0] + ax * ay * m[y1 * tiles_w + x1];
}

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
"  --focus-metric    print one autofocus line \"focus score=.. hfr=.. stars=.. sharp=..\"\n"
"                    (score to MAXIMISE, median HFR of the brightest stars) and stop\n"
"  --margin PX       ignore blobs within PX of the edge, full-res (default 16)\n"
"  --tile PX         background tile size at the DECODED scale (default 64).\n"
"                    Background and noise are measured per tile and\n"
"                    interpolated, so vignetting and light-pollution gradients\n"
"                    do not swamp the detection threshold.\n"
"  --y-origin O      'bottom' (FITS convention, the default, and what\n"
"                    astrometry.net's own .xyls files use) or 'top' (raw image\n"
"                    rows). Getting this wrong flips the parity and the\n"
"                    reported roll angle.\n"
"  --stats           print a summary to stderr\n"
"\n"
"Output: one star per line, \"x y flux\", brightest first, in FULL-RESOLUTION\n"
"pixel coordinates regardless of --downsample.\n", me);
}

/* ------------------------------------------------------------------------
 * EXIF focal length.
 *
 * WHY THIS IS HERE: the pixel scale the solver searches is derived from the
 * focal length, and a wrong focal makes the solve IMPOSSIBLE rather than
 * merely slow -- a 70 mm frame searched at 400 mm looks for 2.3 arcsec/pix
 * when the truth is 13.4, so no quad can ever match. That is not theoretical:
 * a whole night of frames failed to solve for exactly this reason, with a
 * config that said 400 and a zoom lens sitting at 70.
 *
 * A config value cannot track a zoom lens. The frame itself knows, so ask it.
 * We are already decoding this JPEG, so the marker costs one extra read.
 *
 * Deliberately minimal: IFD0 -> ExifIFD -> FocalLength (0x920A, RATIONAL).
 * Every read is bounds-checked against the marker length; a malformed or
 * absent tag simply yields 0 and the caller falls back.
 * ---------------------------------------------------------------------- */
static unsigned rd16(const unsigned char* p, int be)
{ return be ? (unsigned)(p[0] << 8 | p[1]) : (unsigned)(p[1] << 8 | p[0]); }

static unsigned rd32(const unsigned char* p, int be)
{
    return be ? ((unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3])
              : ((unsigned)p[3] << 24 | (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0]);
}

/* returns focal length in mm, or 0 if not found */
static double exif_focal_mm(const unsigned char* d, unsigned len)
{
    unsigned tiff, ifd, n, i, exififd = 0;
    int be;
    if (len < 14 || memcmp(d, "Exif\0\0", 6) != 0) return 0.0;
    tiff = 6;
    if (d[tiff] == 'M' && d[tiff + 1] == 'M') be = 1;
    else if (d[tiff] == 'I' && d[tiff + 1] == 'I') be = 0;
    else return 0.0;
    if (tiff + 8 > len) return 0.0;
    ifd = tiff + rd32(d + tiff + 4, be);

    /* IFD0: we only want the pointer to the Exif sub-IFD */
    if (ifd + 2 > len || ifd < tiff) return 0.0;
    n = rd16(d + ifd, be);
    for (i = 0; i < n; i++) {
        unsigned e = ifd + 2 + i * 12;
        if (e + 12 > len) break;
        if (rd16(d + e, be) == 0x8769) { exififd = tiff + rd32(d + e + 8, be); break; }
    }
    if (!exififd || exififd + 2 > len || exififd < tiff) return 0.0;

    n = rd16(d + exififd, be);
    for (i = 0; i < n; i++) {
        unsigned e = exififd + 2 + i * 12, off, num, den;
        if (e + 12 > len) break;
        if (rd16(d + e, be) != 0x920A) continue;      /* FocalLength */
        if (rd16(d + e + 2, be) != 5) return 0.0;     /* must be RATIONAL */
        off = tiff + rd32(d + e + 8, be);
        if (off + 8 > len || off < tiff) return 0.0;
        num = rd32(d + off, be);
        den = rd32(d + off + 4, be);
        if (!den) return 0.0;
        return (double)num / (double)den;
    }
    return 0.0;
}

int main(int argc, char** argv) {
    const char* fn = NULL;
    int ds = 4, minpix = 3, maxpix = 500, maxstars = 300, margin = 16, stats = 0;
    int gray_pgm = 0;   /* dump the decoded grayscale as a binary PGM, no stars */
    int focus_metric = 0;   /* print one sharpness line for autofocus, no stars */
    double focal_mm = 0.0;                 /* from EXIF; 0 = unknown */
    char focalbuf[32];
    int y_bottom = 1;                 /* FITS convention by default */
    double ksigma = 5.0;
    int i;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE* f;
    unsigned char* gray = NULL;
    int W, H, fullW, fullH;
    JSAMPARRAY buf;
    double bg = 0, noise = 1;
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
        else if (!strcmp(a, "--tile"))       tile = atoi(NEXT());
        else if (!strcmp(a, "--stats"))      stats = 1;
        else if (!strcmp(a, "--gray-pgm"))   gray_pgm = 1;
        else if (!strcmp(a, "--focus-metric")) focus_metric = 1;
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
    /* keep APP1 (EXIF) -- must be requested BEFORE read_header or it is dropped */
    jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
    jpeg_read_header(&cinfo, TRUE);
    {
        jpeg_saved_marker_ptr m;
        for (m = cinfo.marker_list; m; m = m->next) {
            if (m->marker == JPEG_APP0 + 1) {
                focal_mm = exif_focal_mm(m->data, m->data_length);
                if (focal_mm > 0) break;
            }
        }
    }
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

    /* --gray-pgm: emit the decoded grayscale as a binary P5 PGM and stop. This
     * is the pixel source for the Alpaca camera device (polaris-httpd shells out
     * to it, reads the PGM, and serves it as an ImageArray). */
    if (gray_pgm) {
        printf("P5\n%d %d\n255\n", W, H);
        fwrite(gray, 1, (size_t)W * H, stdout);
        free(gray);
        return 0;
    }

    /* Background and noise, ESTIMATED LOCALLY.
     *
     * A global estimate is fine on a clean test image and useless on a real
     * one: a night frame has vignetting, a light-pollution gradient and amp
     * glow, so a whole-frame MAD measures the GRADIENT rather than the pixel
     * noise. On a real R5 II frame that produced bg=84, sigma=31, a threshold
     * of 240/255, and four detected stars. So: split the image into tiles,
     * take a median and MAD per tile, and interpolate between tile centres. */
    {
        int tw = (W + tile - 1) / tile, th = (H + tile - 1) / tile;
        int tx, ty, x, y;
        long hist[256];
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        bgmap = (float*)malloc((size_t)tw * th * sizeof(float));
        nsmap = (float*)malloc((size_t)tw * th * sizeof(float));
        if (!bgmap || !nsmap) { fprintf(stderr, "out of memory\n"); return 1; }
        for (ty = 0; ty < th; ty++) {
            for (tx = 0; tx < tw; tx++) {
                int x0 = tx * tile, y0 = ty * tile;
                int x1 = x0 + tile, y1 = y0 + tile;
                long n = 0, acc, half;
                double med = 0, mad = 1;
                if (x1 > W) x1 = W;
                if (y1 > H) y1 = H;
                memset(hist, 0, sizeof(hist));
                for (y = y0; y < y1; y++)
                    for (x = x0; x < x1; x++) { hist[gray[(size_t)y * W + x]]++; n++; }
                if (!n) { bgmap[ty * tw + tx] = 0; nsmap[ty * tw + tx] = 1; continue; }
                acc = 0; half = n / 2;
                for (i = 0; i < 256; i++) { acc += hist[i]; if (acc >= half) { med = i; break; } }
                {   /* MAD about this tile's own median */
                    long h2[256];
                    memset(h2, 0, sizeof(h2));
                    for (y = y0; y < y1; y++)
                        for (x = x0; x < x1; x++)
                            h2[(int)fabs((double)gray[(size_t)y * W + x] - med)]++;
                    acc = 0;
                    for (i = 0; i < 256; i++) { acc += h2[i]; if (acc >= half) { mad = i; break; } }
                }
                mad *= 1.4826;
                if (mad < 0.8) mad = 0.8;      /* a floor: never trust sigma=0 */
                bgmap[ty * tw + tx] = (float)med;
                nsmap[ty * tw + tx] = (float)mad;
            }
        }
        tiles_w = tw; tiles_h = th;
        /* report the frame-centre values so --stats stays meaningful */
        bg = bgmap[(th / 2) * tw + (tw / 2)];
        noise = nsmap[(th / 2) * tw + (tw / 2)];
    }

    /* Connected components above threshold, iterative flood fill. */
    {
        int cap = 4096;
        int x, y;
        label = (int*)calloc((size_t)W * H, sizeof(int));
        stack = (int*)malloc((size_t)cap * sizeof(int));
        stars = (star_t*)malloc((size_t)(maxstars * 8 + 1024) * sizeof(star_t));
        int starcap = maxstars * 8 + 1024;
        /* Per-blob pixel record, so HFR (flux-weighted mean radius) can be
         * computed in a second pass once the centroid is known. Bounded by
         * maxpix -- a blob larger than that is dropped, not measured. */
        int*    bpx = (int*)   malloc((size_t)(maxpix + 2) * sizeof(int));
        double* bpv = (double*)malloc((size_t)(maxpix + 2) * sizeof(double));
        if (!label || !stack || !stars || !bpx || !bpv) { fprintf(stderr, "out of memory\n"); return 1; }
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                size_t idx = (size_t)y * W + x;
                int sp = 0, npix = 0;
                double sx = 0, sy = 0, sf = 0;
                int minx, maxx, miny, maxy;
                if (label[idx] || gray[idx] <= map_at(bgmap, x, y) + ksigma * map_at(nsmap, x, y))
                    continue;
                label[idx] = 1; stack[sp++] = (int)idx;
                minx = maxx = x; miny = maxy = y;
                while (sp > 0) {
                    int cur = stack[--sp];
                    int cx = cur % W, cy = cur / W;
                    double v = gray[cur] - map_at(bgmap, cx, cy);
                    int dx, dy;
                    if (v < 0) v = 0;
                    npix++; sx += cx * v; sy += cy * v; sf += v;
                    if (npix <= maxpix + 1) { bpx[npix-1] = cur; bpv[npix-1] = v; }
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
                            if (label[nidx] ||
                                gray[nidx] <= map_at(bgmap, nx, ny) + ksigma * map_at(nsmap, nx, ny))
                                continue;
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
                    /* HFR: flux-weighted mean radius of the blob's pixels about
                     * its centroid, in full-resolution px. Small = tight star =
                     * sharp focus. This is the number autofocus minimises. */
                    { double cx0 = sx / sf, cy0 = sy / sf, hnum = 0; int k;
                      for (k = 0; k < npix; k++) {
                          double drx = (bpx[k] % W) - cx0, dry = (bpx[k] / W) - cy0;
                          hnum += bpv[k] * sqrt(drx*drx + dry*dry);
                      }
                      stars[nstars].hfr = (hnum / sf) * ds; }
                    stars[nstars].x = fx; stars[nstars].y = fy;
                    stars[nstars].flux = sf; stars[nstars].npix = npix;
                    nstars++;
                }
            }
        }
        free(bpx); free(bpv);
    }

    qsort(stars, nstars, sizeof(star_t), cmp_flux);
    if (nstars > maxstars) nstars = maxstars;

    /* Autofocus metric: one line, no star list. The number to MAXIMISE is
     * `score`. Near focus, stars are many and tight, so score rises as the
     * median HFR of the brightest stars falls. In gross defocus every star is
     * smeared below threshold and there is no HFR at all; there `score` falls
     * back to a whole-frame gradient sharpness, which still climbs toward focus
     * -- so a sweep can find its way from a blurred field into the star regime,
     * then home in on the sharpest point. */
    if (focus_metric) {
        int nb = nstars < 30 ? nstars : 30, k;
        double medhfr = -1, sharp = 0, score;
        if (nb >= 1) {
            double *hv = (double*)malloc((size_t)nb * sizeof(double));
            if (hv) {
                for (k = 0; k < nb; k++) hv[k] = stars[k].hfr;
                for (k = 1; k < nb; k++) {                 /* insertion sort, tiny n */
                    double t = hv[k]; int j = k - 1;
                    while (j >= 0 && hv[j] > t) { hv[j+1] = hv[j]; j--; }
                    hv[j+1] = t;
                }
                medhfr = (nb & 1) ? hv[nb/2] : 0.5 * (hv[nb/2 - 1] + hv[nb/2]);
                free(hv);
            }
        }
        {   /* mean |gradient| over the decoded gray -- always computable */
            long cnt = 0; double acc = 0; int x2, y2;
            for (y2 = 1; y2 < H - 1; y2++)
                for (x2 = 1; x2 < W - 1; x2++) {
                    size_t p = (size_t)y2 * W + x2;
                    int gx = (int)gray[p+1] - gray[p-1]; if (gx < 0) gx = -gx;
                    int gy = (int)gray[p+W] - gray[p-W]; if (gy < 0) gy = -gy;
                    acc += gx + gy; cnt++;
                }
            sharp = cnt ? acc / (double)cnt : 0;
        }
        /* Score to MAXIMISE. The star term (many, tight stars) is multiplied by
         * the whole-frame sharpness so gross defocus -- where a handful of bright
         * star CORES can still read as deceptively tight HFR -- cannot fake a
         * secondary peak: `sharp` falls monotonically all the way out, dragging
         * the product down with it. With no stars at all, score IS sharp, so a
         * sweep still climbs out of a blurred field toward where stars appear. */
        if (nb >= 3 && medhfr > 0.01) score = (1000.0 * nb / medhfr) * sharp;
        else                          score = sharp;
        printf("focus score=%.4f hfr=%.4f stars=%d sharp=%.4f\n", score, medhfr, nstars, sharp);
        free(gray); free(label); free(stack); free(stars); free(bgmap); free(nsmap);
        return 0;
    }

    gettimeofday(&tv1, NULL);
    if (focal_mm > 0) snprintf(focalbuf, sizeof focalbuf, "%.1fmm(exif)", focal_mm);
    else              snprintf(focalbuf, sizeof focalbuf, "unknown");
    if (stats)
        fprintf(stderr,
                "[extract] %dx%d decoded at 1/%d (full %dx%d), bg=%.1f noise=%.2f "
                "thresh=%.1f stars=%d focal=%s  %.3f s\n",
                W, H, ds, fullW, fullH, bg, noise, bg + ksigma * noise, nstars,
                focalbuf,
                (tv1.tv_sec - tv0.tv_sec) + (tv1.tv_usec - tv0.tv_usec) * 1e-6);

    printf("# polaris-extract: %s\n# full resolution %d x %d\n# y-origin: %s\n",
           fn, fullW, fullH, y_bottom ? "bottom (FITS)" : "top (image)");
    /* The caller reads this to pick the pixel-scale search range. Absent for
     * live-view frames, which carry no EXIF -- the caller falls back there. */
    if (focal_mm > 0)
        printf("# exif focal-mm %.4f\n", focal_mm);
    for (i = 0; i < nstars; i++) {
        double yy = y_bottom ? (fullH - 1 - stars[i].y) : stars[i].y;
        printf("%.3f %.3f %.1f\n", stars[i].x, yy, stars[i].flux);
    }

    free(gray); free(label); free(stack); free(stars); free(bgmap); free(nsmap);
    return nstars > 0 ? 0 : 3;
}
