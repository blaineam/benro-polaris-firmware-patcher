/* SPDX-License-Identifier: MIT
 *
 * polaris-jog — see polaris-jog.h for the hazard this exists to contain.
 */
#define _GNU_SOURCE
#include "polaris-jog.h"
#include "polaris-link.h"

#include <stdio.h>
#include <string.h>

/* Opcode pairs per axis, from docs/APP-PROTOCOL.md. */
static const struct { int fast; int slow; const char *name; } AXIS[JOG_AXES] = {
    { 513, 532, "pan"  },   /* HADJ, M1 */
    { 514, 533, "tilt" },   /* VADJ, M2 */
    { 521, 534, "rot"  },   /* RADJ, M3 */
};

typedef struct {
    int    speed;        /* fast-jog speed, signed; 0 when not in fast mode  */
    int    gear;         /* slow-jog gear, signed; 0 when not in slow mode   */
    int    pressed;      /* a latched state:1 is outstanding                 */
    double lease_ms;     /* absolute monotonic time the intent expires       */
    double next_send_ms; /* absolute monotonic time of the next fast repeat  */
} axis_state_t;

static axis_state_t g_ax[JOG_AXES];

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* --------------------------------------------------------------- wire ---- */

static void send_fast(jog_axis_t a, int speed) {
    char args[64];
    snprintf(args, sizeof args, "speed:%d;", speed);
    plink_send(AXIS[a].fast, PLINK_SUB_CONTROL, args);
}

/* key carries the direction (0 = plus, 1 = minus); level is magnitude 1..5.
 * Level 0 is never sent: it disengages torque rather than stopping (CTL:913),
 * which on a tilt axis means the payload falls rather than holds. */
static void send_slow(jog_axis_t a, int gear, int state) {
    char args[64];
    int key = gear < 0 ? 1 : 0;
    int lvl = clampi(gear < 0 ? -gear : gear, JOG_GEAR_MIN, JOG_GEAR_MAX);
    snprintf(args, sizeof args, "key:%d;state:%d;level:%d;", key, state, lvl);
    plink_send(AXIS[a].slow, PLINK_SUB_CONTROL, args);
}

/* -------------------------------------------------------------- public ---- */

int jog_set(jog_axis_t axis, int speed, int gear) {
    axis_state_t *s;
    double now = plink_now_ms();

    if (axis < 0 || axis >= JOG_AXES) return -1;
    if (speed == 0 && gear == 0) { jog_stop(axis); return 0; }
    if (plink_status() != PLINK_UP) return -1;

    s = &g_ax[axis];

    if (speed != 0) {
        int mag = speed < 0 ? -speed : speed;
        int sgn = speed < 0 ? -1 : 1;
        int want = sgn * clampi(mag, JOG_SPEED_MIN, JOG_SPEED_MAX);

        /* Changing family mid-hold: release the latch first, or the slow press
         * stays outstanding underneath the fast stream and survives the stop. */
        if (s->pressed) { send_slow(axis, s->gear, 0); s->pressed = 0; s->gear = 0; }

        if (s->speed != want) {          /* send immediately on any change */
            s->speed = want;
            send_fast(axis, want);
            s->next_send_ms = now + JOG_REPEAT_MS;
        }
        s->lease_ms = now + JOG_LEASE_MS;
        return 0;
    }

    /* Slow / latched. */
    if (s->speed != 0) { send_fast(axis, 0); s->speed = 0; }

    if (!s->pressed || s->gear != gear) {
        /* A direction or gear change needs the old press released first: the
         * head latches on state:1, so pressing again without releasing leaves
         * an unmatched press behind. */
        if (s->pressed) send_slow(axis, s->gear, 0);
        s->gear = gear;
        send_slow(axis, gear, 1);
        s->pressed = 1;
    }
    /* Deliberately NOT re-pressed on renewal: it is latched, so a repeat press
     * is not a no-op. The lease alone keeps it alive. */
    s->lease_ms = now + JOG_LEASE_MS;
    s->next_send_ms = 0;
    return 0;
}

void jog_stop(jog_axis_t axis) {
    axis_state_t *s;
    if (axis < 0 || axis >= JOG_AXES) return;
    s = &g_ax[axis];

    /* Send the stop even if we believe the axis is already stopped when the
     * link has just come back: our idea of "already stopped" is exactly the
     * thing a dropped link makes untrustworthy. Cheap, idempotent, worth it. */
    if (s->pressed) { send_slow(axis, s->gear, 0); s->pressed = 0; }
    if (s->speed != 0) { send_fast(axis, 0); s->speed = 0; }

    s->gear = 0;
    s->lease_ms = 0;
    s->next_send_ms = 0;
}

void jog_stop_all(void) {
    int i;
    for (i = 0; i < JOG_AXES; i++) jog_stop((jog_axis_t)i);
}

void jog_tick(void) {
    double now = plink_now_ms();
    int i;

    /* A link that went away takes every intent with it. Clearing state without
     * sending is right here: the frames cannot reach the head anyway, and
     * keeping "moving" latched would make the next reconnect think a press is
     * still outstanding. */
    if (plink_status() != PLINK_UP) {
        for (i = 0; i < JOG_AXES; i++) memset(&g_ax[i], 0, sizeof g_ax[i]);
        return;
    }

    for (i = 0; i < JOG_AXES; i++) {
        axis_state_t *s = &g_ax[i];
        if (!s->pressed && s->speed == 0) continue;

        /* THE DEAD MAN. No renewal in JOG_LEASE_MS means whoever asked for this
         * motion is gone -- tab closed, wifi dropped, phone locked -- and the
         * head is not known to stop by itself. */
        if (now >= s->lease_ms) { jog_stop((jog_axis_t)i); continue; }

        if (s->speed != 0 && now >= s->next_send_ms) {
            send_fast((jog_axis_t)i, s->speed);
            s->next_send_ms = now + JOG_REPEAT_MS;
        }
    }
}

double jog_next_deadline_ms(void) {
    double now = plink_now_ms(), best = -1;
    int i;
    for (i = 0; i < JOG_AXES; i++) {
        axis_state_t *s = &g_ax[i];
        double d;
        if (!s->pressed && s->speed == 0) continue;
        d = s->lease_ms - now;                       /* the stop must not be late */
        if (s->speed != 0) {
            double r = s->next_send_ms - now;
            if (r < d) d = r;
        }
        if (d < 0) d = 0;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

int jog_active(void) {
    int i;
    for (i = 0; i < JOG_AXES; i++)
        if (g_ax[i].pressed || g_ax[i].speed != 0) return 1;
    return 0;
}

int jog_status_json(char *out, int cap) {
    double now = plink_now_ms();
    int n = 0, i;
    n += snprintf(out + n, cap - n, "{\"active\":%s,\"axes\":{", jog_active() ? "true" : "false");
    for (i = 0; i < JOG_AXES; i++) {
        const axis_state_t *s = &g_ax[i];
        double left = (s->pressed || s->speed) ? s->lease_ms - now : 0;
        n += snprintf(out + n, cap - n,
                      "%s\"%s\":{\"speed\":%d,\"gear\":%d,\"lease_ms\":%.0f}",
                      i ? "," : "", AXIS[i].name, s->speed, s->gear,
                      left > 0 ? left : 0);
    }
    n += snprintf(out + n, cap - n, "}}");
    return n;
}
