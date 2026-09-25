/* SPDX-License-Identifier: MIT
 *
 * polaris-astro — see polaris-astro.h.
 */
#define _GNU_SOURCE
#include "polaris-astro.h"
#include "polaris-link.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define AHRS_MS 4000.0    /* re-arm inside the head's ~5 s heartbeat budget */

static struct {
    int    active;
    double lat, lon;
    double next_ahrs_ms;
    double align_deg;     /* last heading sent, NAN if none                 */
    int    have_align;
    int    track;         /* last tracking request                          */
    int    rate;
    int    half;
} A;

static int fail(char *err, size_t cap, const char *m) {
    if (err && cap) snprintf(err, cap, "%s", m);
    return -1;
}

int astro_active(void) { return A.active; }

int astro_enter(double lat, double lon, char *err, size_t errcap) {
    if (plink_status() != PLINK_UP) return fail(err, errcap, "control link is down");
    A.lat = lat; A.lon = lon;
    if (!A.active) {
        A.active = 1;
        A.track = 0; A.half = 0; A.rate = ASTRO_RATE_SIDEREAL;
        /* Mode first, then AHRS -- the app enables 520 only after entering the
         * direction-dependent mode. */
        plink_send(ASTRO_CMD_MODE, 2, "mode:8;");
        plink_send(ASTRO_CMD_AHRS, 2, "state:1;");
        A.next_ahrs_ms = plink_now_ms() + AHRS_MS;
        fprintf(stderr, "[astro] session open (mode 8, AHRS on)\n");
    }
    return 0;
}

void astro_leave(void) {
    if (!A.active) return;
    /* Cannot leave mode 8 while tracking, and leaving AHRS on drains the
     * battery -- so unwind in the reverse order it was set up. */
    if (A.track && plink_status() == PLINK_UP)
        plink_send(ASTRO_CMD_TRACK, 3, "state:0;speed:0;");
    if (plink_status() == PLINK_UP) {
        plink_send(ASTRO_CMD_AHRS, 2, "state:0;");
        plink_send(ASTRO_CMD_MODE, 2, "mode:1;");   /* back to plain photo */
    }
    fprintf(stderr, "[astro] session closed\n");
    memset(&A, 0, sizeof A);
}

int astro_align(double compass_deg, char *err, size_t errcap) {
    char args[128];
    if (plink_status() != PLINK_UP) return fail(err, errcap, "control link is down");
    if (!A.active) return fail(err, errcap, "not in astro mode");
    if (!(compass_deg >= 0 && compass_deg <= 360))
        return fail(err, errcap, "heading must be 0..360 degrees");
    /* Match the app's serialisation exactly: compass;lat;lng. lat/lon are the
     * server's configured site -- the same ones the solver uses -- so alignment
     * and solving agree about where the head is. */
    snprintf(args, sizeof args, "compass:%.4f;lat:%.6f;lng:%.6f;",
             compass_deg, A.lat, A.lon);
    if (plink_send(ASTRO_CMD_YAW, 3, args) != 0)
        return fail(err, errcap, "could not send the heading");
    A.align_deg = compass_deg;
    A.have_align = 1;
    fprintf(stderr, "[astro] compass aligned to %.2f deg\n", compass_deg);
    return 0;
}

int astro_track(int on, int rate, int half, char *err, size_t errcap) {
    char args[64];
    if (plink_status() != PLINK_UP) return fail(err, errcap, "control link is down");
    if (!A.active) return fail(err, errcap, "not in astro mode");
    if (on && rate != ASTRO_RATE_SIDEREAL && rate != ASTRO_RATE_LUNAR)
        return fail(err, errcap, "rate must be sidereal (0) or lunar (2)");

    if (on) {
        /* Half-rate is a SEPARATE command, and the wire spelling is inverted
         * from the obvious reading: halfSpeed:1 = half-rate ON. Send it before
         * starting so the head begins at the intended rate. */
        snprintf(args, sizeof args, "halfSpeed:%d;", half ? 1 : 0);
        plink_send(ASTRO_CMD_HALF, 3, args);
        snprintf(args, sizeof args, "state:1;speed:%d;", rate);
        if (plink_send(ASTRO_CMD_TRACK, 3, args) != 0)
            return fail(err, errcap, "could not start tracking");
        A.track = 1; A.rate = rate; A.half = half ? 1 : 0;
    } else {
        if (plink_send(ASTRO_CMD_TRACK, 3, "state:0;speed:0;") != 0)
            return fail(err, errcap, "could not stop tracking");
        A.track = 0;
    }
    return 0;
}

void astro_tick(void) {
    double now;
    if (!A.active) return;
    if (plink_status() != PLINK_UP) {
        /* The session cannot survive a link loss: the AHRS heartbeat stops, the
         * frame goes stale, and we can no longer stop tracking. Fold it. */
        fprintf(stderr, "[astro] link lost — closing the astro session\n");
        memset(&A, 0, sizeof A);
        return;
    }
    now = plink_now_ms();
    if (now >= A.next_ahrs_ms) {
        A.next_ahrs_ms = now + AHRS_MS;
        plink_send(ASTRO_CMD_AHRS, 2, "state:1;");
    }
}

double astro_next_deadline_ms(void) {
    double d;
    if (!A.active) return -1;
    d = A.next_ahrs_ms - plink_now_ms();
    return d < 0 ? 0 : d;
}

int astro_status_json(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n, "{\"active\":%s", A.active ? "true" : "false");
    if (A.active) {
        n += snprintf(out + n, cap - n,
            ",\"lat\":%.6f,\"lon\":%.6f,\"track_requested\":%s,\"rate\":%d,\"half\":%s",
            A.lat, A.lon, A.track ? "true" : "false", A.rate, A.half ? "true" : "false");
        if (A.have_align)
            n += snprintf(out + n, cap - n, ",\"align_deg\":%.2f", A.align_deg);
    }
    return n + snprintf(out + n, cap - n, "}");
}
