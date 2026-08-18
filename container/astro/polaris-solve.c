/* SPDX-License-Identifier: MIT
 *
 * polaris-solve — a minimal astrometry.net front end for the Benro Polaris.
 *
 * Takes a star list (ours, or an astrometry.net .xyls) plus the lens/sensor
 * geometry, and prints the solved sky position as one JSON line. It is a
 * SEPARATE PROCESS on purpose: astrometry.net's FITS layer is GPL, so the
 * licence boundary is the process boundary (see docs/LICENSE-AUDIT.md).
 *
 * Nothing here reads pixels — star extraction is somebody else's job, which is
 * also what keeps GPLv3 `simplexy`/`ctmf` out of the build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>

#include "os-features.h"
#include "solver.h"
#include "index.h"
#include "starxy.h"
#include "matchobj.h"
#include "sip.h"
#include "sip_qfits.h"
#include "sip-utils.h"
#include "xylist.h"
#include "log.h"
#include "errors.h"
#include "starutil.h"
#include "mathutil.h"
#include "fitsioutils.h"

#define MAXIDX 64

/* A WRONG hint is far worse than no hint: the solver searches the hinted region
 * first, finds nothing, and then grinds. Measured on device: a mismatched hint
 * ran 11 minutes before being killed. The align loop must never be able to hang
 * the mount like that, so enforce a hard wall-clock bound with SIGALRM -- it
 * cannot be defeated by anything inside the solver. */
static volatile int g_nfield_for_timeout = 0;
static void on_timeout(int sig) {
    const char* msg;
    (void)sig;
    msg = "{\"solved\":false,\"error\":\"timeout\"}\n";
    /* write(), not printf(): async-signal-safe */
    if (write(1, msg, strlen(msg)) < 0) { /* nothing useful to do */ }
    _exit(3);
}

static void usage(const char* me) {
    fprintf(stderr,
"usage: %s --index <file-or-dir> [...] (--stars FILE | --xyls FILE)\n"
"          --width W --height H [geometry] [hints]\n"
"\n"
"  star input\n"
"    --stars FILE      plain text, one star per line: \"x y [flux]\"\n"
"                      (0,0 = top-left; flux optional, brightest first if absent)\n"
"    --xyls FILE       an astrometry.net .xyls table (X, Y, FLUX columns)\n"
"    --width/--height  image size in pixels (REQUIRED)\n"
"\n"
"  geometry (either form; both set the pixel-scale search range)\n"
"    --focal-mm F      lens focal length\n"
"    --sensor-mm S     sensor width         (default 36.0, full frame)\n"
"    --scale-tol T     fractional tolerance (default 0.20 = +/-20%%)\n"
"    --scale-low A --scale-high B   arcsec/pixel, set directly\n"
"\n"
"  hints (optional, big speedups)\n"
"    --ra R --dec D --radius DEG    search near this sky position only\n"
"    --depth N         stop after the brightest N stars (default 0 = all)\n"
"    --cpulimit SEC    hard wall-clock limit; prints {\"solved\":false,\n"
"                      \"error\":\"timeout\"} and exits 3 (default 0 = no limit).\n"
"                      ALWAYS set this in an automated loop: a wrong hint can\n"
"                      grind for many minutes.\n"
"    --parity 0|1|2    0=normal 1=flipped 2=both (default 2)\n"
"    --no-tweak        skip the 2nd-order SIP refinement (TAN fit only)\n"
"    --verbose\n", me);
}

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

/* "x y [flux]" per line; '#' comments and blank lines skipped. */
static starxy_t* read_star_text(const char* fn) {
    FILE* f = fopen(fn, "r");
    char line[512];
    double *x = NULL, *y = NULL, *flux = NULL;
    int n = 0, cap = 0, anyflux = 0;
    starxy_t* sxy;
    if (!f) { fprintf(stderr, "cannot open %s\n", fn); return NULL; }
    while (fgets(line, sizeof(line), f)) {
        double xx, yy, ff = 0.0;
        int got;
        if (line[0] == '#' || line[0] == '\n') continue;
        got = sscanf(line, "%lf %lf %lf", &xx, &yy, &ff);
        if (got < 2) continue;
        if (got >= 3) anyflux = 1;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            x = realloc(x, cap * sizeof(double));
            y = realloc(y, cap * sizeof(double));
            flux = realloc(flux, cap * sizeof(double));
        }
        x[n] = xx; y[n] = yy; flux[n] = (got >= 3) ? ff : (double)(cap - n);
        n++;
    }
    fclose(f);
    if (!n) { fprintf(stderr, "no stars in %s\n", fn); free(x); free(y); free(flux); return NULL; }
    sxy = starxy_new(n, TRUE, FALSE);
    starxy_set_x_array(sxy, x);
    starxy_set_y_array(sxy, y);
    starxy_set_flux_array(sxy, flux);
    if (anyflux) starxy_sort_by_flux(sxy);
    free(x); free(y); free(flux);
    return sxy;
}

static starxy_t* read_star_xyls(const char* fn) {
    xylist_t* xy = xylist_open(fn);
    starxy_t* sxy;
    if (!xy) { fprintf(stderr, "cannot open xyls %s\n", fn); return NULL; }
    sxy = xylist_read_field(xy, NULL);
    xylist_close(xy);
    if (!sxy) { fprintf(stderr, "cannot read field from %s\n", fn); return NULL; }
    starxy_sort_by_flux(sxy);
    return sxy;
}

/* Add an index file, or every index file in a directory. */
static int add_indexes(solver_t* solver, const char* path, int verbose) {
    index_t* ind = index_load(path, 0, NULL);
    if (ind) {
        solver_add_index(solver, ind);
        if (verbose) fprintf(stderr, "[polaris-solve] index %s\n", ind->indexname);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    solver_t* solver;
    starxy_t* field = NULL;
    const char* idxpaths[MAXIDX];
    int nidx = 0, i;
    const char *starsfn = NULL, *xylsfn = NULL;
    double W = 0, H = 0;
    double focal = 0, sensor = 36.0, tol = 0.20;
    double slo = 0, shi = 0;
    double ra = 0, dec = 0, radius = 0;
    int have_radec = 0, depth = 0, parity = 2, verbose = 0, tweak = 1;
    double cpulimit = 0, t0, dt;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (usage(argv[0]), exit(2), ""))
        if      (!strcmp(a, "--index"))      { if (nidx < MAXIDX) idxpaths[nidx++] = NEXT(); }
        else if (!strcmp(a, "--stars"))      starsfn = NEXT();
        else if (!strcmp(a, "--xyls"))       xylsfn  = NEXT();
        else if (!strcmp(a, "--width"))      W = atof(NEXT());
        else if (!strcmp(a, "--height"))     H = atof(NEXT());
        else if (!strcmp(a, "--focal-mm"))   focal = atof(NEXT());
        else if (!strcmp(a, "--sensor-mm"))  sensor = atof(NEXT());
        else if (!strcmp(a, "--scale-tol"))  tol = atof(NEXT());
        else if (!strcmp(a, "--scale-low"))  slo = atof(NEXT());
        else if (!strcmp(a, "--scale-high")) shi = atof(NEXT());
        else if (!strcmp(a, "--ra"))         { ra = atof(NEXT()); have_radec = 1; }
        else if (!strcmp(a, "--dec"))        { dec = atof(NEXT()); have_radec = 1; }
        else if (!strcmp(a, "--radius"))     radius = atof(NEXT());
        else if (!strcmp(a, "--depth"))      depth = atoi(NEXT());
        else if (!strcmp(a, "--cpulimit"))   cpulimit = atof(NEXT());
        else if (!strcmp(a, "--parity"))     parity = atoi(NEXT());
        else if (!strcmp(a, "--no-tweak"))   tweak = 0;
        else if (!strcmp(a, "--verbose"))    verbose = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 2; }
        #undef NEXT
    }
    if (!nidx || (!starsfn && !xylsfn) || W <= 0 || H <= 0) { usage(argv[0]); return 2; }

    log_init(verbose ? LOG_VERB : LOG_MSG);
    log_to(stderr);          /* keep stdout clean: it carries only our JSON */
    fits_use_error_system();

    field = starsfn ? read_star_text(starsfn) : read_star_xyls(xylsfn);
    if (!field) return 1;

    /* pixel scale search range, in arcsec/pixel */
    if (slo <= 0 || shi <= 0) {
        if (focal > 0) {
            double pixmm = sensor / W;
            double nominal = rad2arcsec(atan(pixmm / focal));
            slo = nominal * (1.0 - tol);
            shi = nominal * (1.0 + tol);
        } else {
            slo = 0.1; shi = 3600.0;              /* blind */
        }
    }

    solver = solver_new();
    solver->funits_lower = slo;
    solver->funits_upper = shi;
    solver->quadsize_min = 0.1 * (W < H ? W : H);
    solver->distance_from_quad_bonus = TRUE;
    solver->parity = (parity == 0) ? PARITY_NORMAL : (parity == 1) ? PARITY_FLIP : PARITY_BOTH;
    if (depth > 0) solver->endobj = depth;
    if (cpulimit > 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_timeout;
        sigaction(SIGALRM, &sa, NULL);
        alarm((unsigned)(cpulimit + 0.5));
    }
    solver_set_keep_logodds(solver, log(1e12));
    /* Refine with a 2nd-order SIP fit. Camera lenses -- especially the wide
     * ones used for an alignment frame -- have enough distortion that a plain
     * TAN fit biases the field centre by a noticeable fraction of a degree,
     * and the centre is exactly what the mount is told to sync to. */
    if (tweak) {
        solver->do_tweak = TRUE;
        solver->tweak_aborder = 2;
        solver->tweak_abporder = 2;
    }
    solver_set_field(solver, field);
    solver_set_field_bounds(solver, 0, W, 0, H);
    if (have_radec && radius > 0) solver_set_radec(solver, ra, dec, radius);

    for (i = 0; i < nidx; i++)
        if (!add_indexes(solver, idxpaths[i], verbose))
            fprintf(stderr, "[polaris-solve] WARNING: could not load index %s\n", idxpaths[i]);
    if (solver_n_indices(solver) == 0) {
        fprintf(stderr, "[polaris-solve] no indexes loaded\n");
        return 1;
    }

    t0 = now_seconds();
    solver_run(solver);
    dt = now_seconds() - t0;

    if (solver->best_match_solves) {
        tan_t* wcs = &(solver->best_match.wcstan);
        double cra, cdec, pscale, orient, det;
        tan_pixelxy2radec(wcs, W / 2.0, H / 2.0, &cra, &cdec);
        pscale = tan_pixel_scale(wcs);
        orient = tan_get_orientation(wcs);
        /* Sign of the CD determinant = handedness of the solution. The caller
         * needs this to turn `roll` into a real camera orientation: a mirrored
         * field rolls the opposite way. */
        det = wcs->cd[0][0] * wcs->cd[1][1] - wcs->cd[0][1] * wcs->cd[1][0];
        printf("{\"solved\":true,"
               "\"ra_deg\":%.6f,\"dec_deg\":%.6f,\"roll_deg\":%.4f,"
               "\"pixscale_arcsec\":%.6f,\"field_w_deg\":%.4f,\"field_h_deg\":%.4f,"
               "\"parity\":\"%s\",\"cd\":[%.9g,%.9g,%.9g,%.9g],"
               "\"index\":\"%s\",\"logodds\":%.3f,\"nmatch\":%i,\"nfield\":%i,"
               "\"solve_seconds\":%.3f}\n",
               cra, cdec, orient, pscale,
               arcsec2deg(pscale * W), arcsec2deg(pscale * H),
               (det < 0) ? "normal" : "flipped",
               wcs->cd[0][0], wcs->cd[0][1], wcs->cd[1][0], wcs->cd[1][1],
               solver->best_index ? solver->best_index->indexname : "?",
               solver->best_match.logodds, solver->best_match.nmatch,
               starxy_n(field), dt);
        return 0;
    }
    printf("{\"solved\":false,\"nfield\":%i,\"solve_seconds\":%.3f}\n",
           starxy_n(field), dt);
    return 3;
}
