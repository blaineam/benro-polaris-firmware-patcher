/* SPDX-License-Identifier: MIT
 *
 * polaris-skysim — a simulated camera. Renders the real sky as seen by a lens
 * of a given focal length pointed at a given RA/Dec/roll, and writes a JPEG.
 *
 * The stars come out of the SAME astrometry.net index files the solver reads,
 * so a render/solve round trip tests the whole chain honestly: no shared
 * assumptions beyond the catalogue itself, and every star in the frame is one
 * the solver could legitimately match.
 *
 * That closes the loop for motor testing without a camera or a sky:
 *
 *     mount pose -> skysim renders what the camera would see
 *                -> polaris-extract -> polaris-solve -> align -> goto
 *                -> mount pose has changed -> render again ...
 *
 *   polaris-skysim --index idx.fits --ra 84.5 --dec -2.7 --focal-mm 400 \
 *                  --width 8192 --height 6144 --out frame.jpg
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <jpeglib.h>

#include "os-features.h"
#include "starkd.h"
#include "starutil.h"
#include "sip.h"
#include "log.h"
#include "errors.h"

#define DEG (M_PI / 180.0)

static void usage(const char* me) {
    fprintf(stderr,
"usage: %s --index FILE [--index FILE ...] --ra D --dec D --out FILE.jpg\n"
"  pointing\n"
"    --ra D --dec D     field centre, J2000 degrees (required)\n"
"    --roll D           camera rotation, degrees (default 0)\n"
"  optics / sensor\n"
"    --focal-mm F       lens focal length          (default 400)\n"
"    --sensor-mm S      sensor width               (default 36, full frame)\n"
"    --width W --height H   pixels                 (default 8192 x 6144)\n"
"  rendering\n"
"    --fwhm PX          star size, pixels          (default 3.5)\n"
"    --background N     sky level, 0-255           (default 22)\n"
"    --noise N          gaussian noise sigma       (default 3)\n"
"    --flux N           peak brightness of a star  (default 200)\n"
"    --seed N           deterministic noise/jitter (default 1)\n"
"    --quality N        jpeg quality               (default 92)\n"
"    --stats            report what was rendered\n", me);
}

/* Deterministic, portable PRNG so a seed always reproduces a frame.
 * MUST be a fixed-width 64-bit type: `unsigned long` is 64-bit on x86-64 but
 * 32-bit on the device's ARM, where a >>33 is undefined behaviour. */
static uint64_t g_rng = 1;
static double frand(void) {
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((g_rng >> 33) & 0x7fffffff) / 2147483648.0;
}
static double gauss(void) {          /* Box-Muller */
    double u1 = frand(), u2 = frand();
    if (u1 < 1e-12) u1 = 1e-12;
    return sqrt(-2.0 * log(u1)) * cos(2 * M_PI * u2);
}

int main(int argc, char** argv) {
    const char* idxfn[16];
    int nidx = 0, i, j;
    double ra = NAN, dec = NAN, roll = 0;
    double focal = 400, sensor = 36;
    int W = 8192, H = 6144, quality = 92, stats = 0;
    double fwhm = 3.5, bg = 22, noise = 3, flux = 200;
    const char* outfn = NULL;
    unsigned char* img;
    tan_t wcs;
    double pixscale_arcsec, radius_deg;
    int nstars_drawn = 0, nfound = 0;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (usage(argv[0]), exit(2), ""))
        if      (!strcmp(a, "--index"))      { if (nidx < 16) idxfn[nidx++] = NEXT(); }
        else if (!strcmp(a, "--ra"))         ra = atof(NEXT());
        else if (!strcmp(a, "--dec"))        dec = atof(NEXT());
        else if (!strcmp(a, "--roll"))       roll = atof(NEXT());
        else if (!strcmp(a, "--focal-mm"))   focal = atof(NEXT());
        else if (!strcmp(a, "--sensor-mm"))  sensor = atof(NEXT());
        else if (!strcmp(a, "--width"))      W = atoi(NEXT());
        else if (!strcmp(a, "--height"))     H = atoi(NEXT());
        else if (!strcmp(a, "--fwhm"))       fwhm = atof(NEXT());
        else if (!strcmp(a, "--background")) bg = atof(NEXT());
        else if (!strcmp(a, "--noise"))      noise = atof(NEXT());
        else if (!strcmp(a, "--flux"))       flux = atof(NEXT());
        else if (!strcmp(a, "--seed"))       g_rng = (uint64_t)strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--quality"))    quality = atoi(NEXT());
        else if (!strcmp(a, "--out"))        outfn = NEXT();
        else if (!strcmp(a, "--stats"))      stats = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 2; }
        #undef NEXT
    }
    if (!nidx || ra != ra || dec != dec || !outfn) { usage(argv[0]); return 2; }

    log_init(LOG_MSG);
    log_to(stderr);

    /* A TAN projection is exactly what the solver will fit, so building the
     * frame with one means any disagreement is a real error rather than a
     * projection mismatch. */
    pixscale_arcsec = rad2arcsec(atan((sensor / W) / focal));
    memset(&wcs, 0, sizeof(wcs));
    wcs.crval[0] = ra;  wcs.crval[1] = dec;
    wcs.crpix[0] = W / 2.0 + 0.5;  wcs.crpix[1] = H / 2.0 + 0.5;
    {
        double s = pixscale_arcsec / 3600.0;
        double cr = cos(roll * DEG), sr = sin(roll * DEG);
        wcs.cd[0][0] = -s * cr;  wcs.cd[0][1] =  s * sr;
        wcs.cd[1][0] =  s * sr;  wcs.cd[1][1] =  s * cr;
    }
    wcs.imagew = W;  wcs.imageh = H;
    radius_deg = hypot(W, H) * pixscale_arcsec / 3600.0 / 2.0 * 1.10;

    img = (unsigned char*)malloc((size_t)W * H);
    if (!img) { fprintf(stderr, "cannot allocate %dx%d\n", W, H); return 1; }
    for (i = 0; i < W * H; i++) {
        double v = bg + gauss() * noise;
        img[i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    for (j = 0; j < nidx; j++) {
        startree_t* st = startree_open(idxfn[j]);
        double* radecs = NULL;
        int* inds = NULL;
        int N = 0, k;
        if (!st) { fprintf(stderr, "cannot open index %s\n", idxfn[j]); continue; }
        startree_search_for_radec(st, ra, dec, radius_deg, NULL, &radecs, &inds, &N);
        nfound += N;
        for (k = 0; k < N; k++) {
            double px, py, peak, sigma;
            int x0, x1, y0, y1, x, y;
            if (!tan_radec2pixelxy(&wcs, radecs[2 * k], radecs[2 * k + 1], &px, &py))
                continue;
            if (px < 0 || py < 0 || px >= W || py >= H) continue;
            /* Brightness varies per star so the extractor has something to sort
             * by; deterministic in the seed, not tied to catalogue order. */
            peak = flux * (0.45 + 0.55 * frand());
            sigma = fwhm / 2.3548;
            x0 = (int)(px - 4 * sigma); x1 = (int)(px + 4 * sigma);
            y0 = (int)(py - 4 * sigma); y1 = (int)(py + 4 * sigma);
            if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
            if (x1 >= W) x1 = W - 1; if (y1 >= H) y1 = H - 1;
            for (y = y0; y <= y1; y++) {
                for (x = x0; x <= x1; x++) {
                    double dx = x + 0.5 - px, dy = y + 0.5 - py;
                    double g = peak * exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
                    int v = img[(size_t)y * W + x] + (int)(g + 0.5);
                    img[(size_t)y * W + x] = (unsigned char)(v > 255 ? 255 : v);
                }
            }
            nstars_drawn++;
        }
        free(radecs); free(inds);
        startree_close(st);
    }

    {
        struct jpeg_compress_struct cinfo;
        struct jpeg_error_mgr jerr;
        FILE* f = fopen(outfn, "wb");
        JSAMPROW row;
        if (!f) { fprintf(stderr, "cannot write %s\n", outfn); return 1; }
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_compress(&cinfo);
        jpeg_stdio_dest(&cinfo, f);
        cinfo.image_width = W; cinfo.image_height = H;
        cinfo.input_components = 1; cinfo.in_color_space = JCS_GRAYSCALE;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, quality, TRUE);
        jpeg_start_compress(&cinfo, TRUE);
        while (cinfo.next_scanline < (unsigned)H) {
            row = img + (size_t)cinfo.next_scanline * W;
            jpeg_write_scanlines(&cinfo, &row, 1);
        }
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
        fclose(f);
    }

    if (stats)
        fprintf(stderr, "[skysim] %dx%d  %.4f arcsec/px  field %.3f x %.3f deg  "
                "catalogue hits %d, drawn %d -> %s\n",
                W, H, pixscale_arcsec,
                W * pixscale_arcsec / 3600.0, H * pixscale_arcsec / 3600.0,
                nfound, nstars_drawn, outfn);
    printf("{\"ra_deg\":%.6f,\"dec_deg\":%.6f,\"roll_deg\":%.4f,"
           "\"pixscale_arcsec\":%.6f,\"field_w_deg\":%.4f,\"field_h_deg\":%.4f,"
           "\"stars\":%d,\"out\":\"%s\"}\n",
           ra, dec, roll, pixscale_arcsec,
           W * pixscale_arcsec / 3600.0, H * pixscale_arcsec / 3600.0,
           nstars_drawn, outfn);
    free(img);
    return nstars_drawn > 0 ? 0 : 4;
}
