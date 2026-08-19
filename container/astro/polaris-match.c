/* ===========================================================================
 * polaris-match -- how far has the field drifted between two star lists?
 *
 *     polaris-match --ref stars_a.txt --cur stars_b.txt [--tol 2.0] [--top 60]
 *     -> {"matched":true,"dx":-3.42,"dy":1.08,"votes":47,"nref":93,"ncur":91}
 *
 * WHY NOT JUST PLATE-SOLVE AGAIN: a solve is 1-4 s and hammers the CPU. Guiding
 * only needs to know how far the SAME field has shifted, which is a translation
 * between two star lists -- milliseconds. Full solves are then reserved for
 * re-anchoring when matching fails (cloud, a slew, drift too large to match).
 *
 * METHOD: Hough-style vote on translation. Every (ref, cur) pair proposes a
 * (dx, dy); the true translation gets a vote from every genuinely matched star
 * while spurious pairs scatter. Take the densest cluster, then refine by
 * averaging the pairs that agree with it.
 *
 * O(N*M) on the brightest --top stars of each list, so it is bounded regardless
 * of how many the extractor found.
 *
 * Rotation is deliberately NOT modelled. Over a guiding interval on an alt-az
 * mount, field rotation is small compared with the translation we are
 * correcting, and fitting it from noisy centroids does more harm than good.
 * If the field has rotated enough to matter, matching degrades and the caller
 * falls back to a full solve -- which is the correct response anyway.
 *
 * MIT.
 * =========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { double x, y, flux; } Star;

static int load(const char *path, Star *s, int cap) {
    FILE *f = fopen(path, "r");
    char line[512];
    int n = 0;
    if (!f) return -1;
    while (n < cap && fgets(line, sizeof line, f)) {
        double x, y, fl = 1.0;
        if (line[0] == '#') continue;
        if (sscanf(line, "%lf %lf %lf", &x, &y, &fl) < 2) continue;
        s[n].x = x; s[n].y = y; s[n].flux = fl;
        n++;
    }
    fclose(f);
    return n;
}

static int by_flux(const void *a, const void *b) {
    double d = ((const Star *)b)->flux - ((const Star *)a)->flux;
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

int main(int argc, char **argv) {
    const char *refp = NULL, *curp = NULL;
    double tol = 2.0;
    int top = 60, i, j, k;
    static Star ref[4096], cur[4096];
    int nref, ncur;
    double bdx = 0, bdy = 0;
    int best = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ref") && i+1 < argc) refp = argv[++i];
        else if (!strcmp(argv[i], "--cur") && i+1 < argc) curp = argv[++i];
        else if (!strcmp(argv[i], "--tol") && i+1 < argc) tol = atof(argv[++i]);
        else if (!strcmp(argv[i], "--top") && i+1 < argc) top = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s --ref A.txt --cur B.txt [--tol PX] [--top N]\n", argv[0]);
            return 2;
        }
    }
    if (!refp || !curp) { fprintf(stderr, "--ref and --cur are required\n"); return 2; }

    nref = load(refp, ref, 4096);
    ncur = load(curp, cur, 4096);
    if (nref <= 0 || ncur <= 0) {
        printf("{\"matched\":false,\"reason\":\"empty star list\",\"nref\":%d,\"ncur\":%d}\n",
               nref, ncur);
        return 1;
    }
    qsort(ref, (size_t)nref, sizeof(Star), by_flux);
    qsort(cur, (size_t)ncur, sizeof(Star), by_flux);
    if (nref > top) nref = top;
    if (ncur > top) ncur = top;

    /* vote: each (ref,cur) pair proposes a translation */
    for (i = 0; i < nref; i++) {
        for (j = 0; j < ncur; j++) {
            double dx = cur[j].x - ref[i].x;
            double dy = cur[j].y - ref[i].y;
            int votes = 0;
            for (k = 0; k < nref; k++) {
                int m;
                for (m = 0; m < ncur; m++) {
                    double ex = (cur[m].x - ref[k].x) - dx;
                    double ey = (cur[m].y - ref[k].y) - dy;
                    if (fabs(ex) <= tol && fabs(ey) <= tol) { votes++; break; }
                }
            }
            if (votes > best) { best = votes; bdx = dx; bdy = dy; }
        }
    }

    if (best < 4) {
        printf("{\"matched\":false,\"reason\":\"too few agreeing stars\","
               "\"votes\":%d,\"nref\":%d,\"ncur\":%d}\n", best, nref, ncur);
        return 1;
    }

    /* refine: average the pairs that agree with the winning translation */
    {
        double sx = 0, sy = 0;
        int n = 0;
        for (i = 0; i < nref; i++) {
            for (j = 0; j < ncur; j++) {
                double dx = cur[j].x - ref[i].x;
                double dy = cur[j].y - ref[i].y;
                if (fabs(dx - bdx) <= tol && fabs(dy - bdy) <= tol) {
                    sx += dx; sy += dy; n++;
                    break;
                }
            }
        }
        if (n) { bdx = sx / n; bdy = sy / n; }
        printf("{\"matched\":true,\"dx\":%.3f,\"dy\":%.3f,\"votes\":%d,"
               "\"refined_from\":%d,\"nref\":%d,\"ncur\":%d}\n",
               bdx, bdy, best, n, nref, ncur);
    }
    return 0;
}
