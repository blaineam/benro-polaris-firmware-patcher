/* SPDX-License-Identifier: MIT
 *
 * polaris-prog — see polaris-prog.h for why the server owns the progress poll.
 */
#define _GNU_SOURCE
#include "polaris-prog.h"
#include "polaris-link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The head answers a remaining-count request and expects the next one straight
 * away. "Straight away" from a phone is as fast as the UI can re-issue it; we
 * pace it, because nothing here changes faster than a frame interval and the AP
 * is shared with the live-view stream. */
#define POLL_MS        1000.0
/* If the head stops answering the remaining-count poll for this long, the run
 * is over as far as we can tell. Do NOT silently call that "complete": it is
 * equally consistent with the camera having died mid-run, and telling someone
 * their 400-frame timelapse finished when it stalled at frame 12 is worse than
 * saying we lost track. */
#define STALE_MS      45000.0

typedef struct {
    prog_kind_t kind;
    double      started_ms;
    double      next_poll_ms;
    double      last_reply_ms;
    long        remaining;      /* -1 unknown, -2 unlimited            */
    long        total;          /* -1 unlimited                        */
    int         paused;
    int         stale;          /* the head stopped answering          */
    char        start_frame[PLINK_ARGS_MAX];  /* what we actually sent  */
    lapse_params_t lapse;
    pano_params_t  pano;
} state_t;

static state_t g;

/* ------------------------------------------------------------------ util */

static int fail(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
    return -1;
}

/* The head wants plain decimals. "%g" would emit 1e+06 for a long unlimited
 * run's derived values and the parser on the other side is a substring scan. */
static void fmt_num(char *out, size_t cap, double v) {
    if (v == floor(v) && fabs(v) < 1e9) snprintf(out, cap, "%ld", (long)v);
    else                                snprintf(out, cap, "%.3f", v);
}

/* ---------------------------------------------------------- validation */

int prog_check_lapse(const lapse_params_t *p, char *err, size_t errcap) {
    if (!p) return fail(err, errcap, "no parameters");
    if (!(p->interval_s > 0) || p->interval_s > 3600)
        return fail(err, errcap, "interval must be between 0 and 3600 seconds");
    if (p->shots != -1 && (p->shots < 1 || p->shots > 100000))
        return fail(err, errcap, "shots must be 1..100000, or unlimited");
    if (p->bulb_s < 0 || p->bulb_s > 3600)
        return fail(err, errcap, "bulb must be 0..3600 seconds");
    /* An exposure longer than the gap between frames cannot work, and the app
     * does not check it at all -- it simply produces a run that silently falls
     * behind. Refusing is kinder than a timelapse that is wrong by morning. */
    if (p->bulb_s > 0 && p->bulb_s >= p->interval_s)
        return fail(err, errcap, "bulb exposure is longer than the interval — "
                                 "frames would overlap; raise the interval");
    if (p->fps < 1 || p->fps > 240)
        return fail(err, errcap, "fps must be 1..240");
    return 0;
}

int prog_check_pano(const pano_params_t *p, char *err, size_t errcap) {
    if (!p) return fail(err, errcap, "no parameters");
    if (p->h_num < 1 || p->h_num > 360) return fail(err, errcap, "columns must be 1..360");
    if (p->v_num < 1 || p->v_num > 180) return fail(err, errcap, "rows must be 1..180");
    if (!(p->h_angle > 0) || p->h_angle > 90)
        return fail(err, errcap, "horizontal step must be between 0 and 90 degrees");
    if (p->v_angle < 0 || p->v_angle > 90)
        return fail(err, errcap, "vertical step must be 0..90 degrees");
    if (p->start_dir < 0 || p->start_dir > 4)
        return fail(err, errcap, "start corner must be 0..4");
    if (p->per_spot < 1 || p->per_spot > 99)
        return fail(err, errcap, "frames per position must be 1..99");
    if (p->interval_s < 0 || p->interval_s > 20)
        return fail(err, errcap, "per-position interval must be 0..20 seconds");
    if (p->bulb_s < 0 || p->bulb_s > 3600)
        return fail(err, errcap, "bulb must be 0..3600 seconds");
    /* The head physically cannot sweep past a full turn; the app's own wheels
     * cap shot count at floor(360/step) for the same reason. */
    if (p->h_angle * p->h_num > 360.0 + 1e-6)
        return fail(err, errcap, "horizontal sweep exceeds 360° — reduce the step or the columns");
    if (p->v_angle * p->v_num > 180.0 + 1e-6)
        return fail(err, errcap, "vertical sweep exceeds 180° — reduce the step or the rows");
    return 0;
}

/* -------------------------------------------------------------- payloads */

/* 272 step 2, static timelapse (StaticLapseLayout.java:239-245):
 *     step:2;point:1;time:0;para:<interval>,<picCount>;bulb:<sec>;
 * picCount -1 means unlimited. */
static void lapse_point_frame(const lapse_params_t *p, char *out, size_t cap) {
    char iv[32], bl[32];
    fmt_num(iv, sizeof iv, p->interval_s);
    fmt_num(bl, sizeof bl, p->bulb_s);
    snprintf(out, cap, "step:%d;point:1;time:0;para:%s,%ld;bulb:%s;",
             LAPSE_STEP_POINT, iv, p->shots, bl);
}

/* 272 step 3 (MainActivity:1206-1212):
 *     step:3;point:<n>;time:<s>;photoCnt:<n>;;preview:<n>;
 * THE DOUBLE SEMICOLON IS REAL -- the app emits it (docs/APP-PROTOCOL.md §2
 * trap 3). Do not "fix" it: this is a substring-scanned protocol and the frame
 * the head has been parsing for years is the one with the empty field. */
static void lapse_end_frame(const lapse_params_t *p, char *out, size_t cap) {
    long total_s = (p->shots == -1) ? -1
                 : (long)((p->shots - 1) * p->interval_s);
    snprintf(out, cap, "step:%d;point:1;time:%ld;photoCnt:%ld;;preview:0;",
             LAPSE_STEP_SEND_END, total_s, p->shots);
}

/* 271 step 2 -- SEE THE HEADER. Field order and units are established; the
 * binding of isp/bgsem/dir is inference because the method that sets them
 * failed to decompile.
 *
 *   para:<hNum>,<vNum>,<startDir>,<paMode>,<hAngle>,<vAngle>;
 *   isp:<0|1>;bgsem:<0|15>;num:<N>;bulb:<sec>;
 *
 * paMode is 0 (normal grid) here: PRO and 720 need a start/end pose flown on
 * the head and an optics model, and neither is wired yet. `gimbal:` and `dir:`
 * are omitted for the same reason -- they belong to those variants, and sending
 * a field whose meaning is undetermined is exactly what this file avoids.
 * bgsem is only ever 0 or 15 in the app and its meaning is undetermined; 0 is
 * the conservative choice. */
static void pano_start_frame(const pano_params_t *p, char *out, size_t cap) {
    char ha[32], va[32], bl[32];
    long num = (long)p->h_num * p->v_num * p->per_spot;
    fmt_num(ha, sizeof ha, p->h_angle);
    fmt_num(va, sizeof va, p->v_angle);
    fmt_num(bl, sizeof bl, p->bulb_s);
    snprintf(out, cap,
             "step:%d;para:%d,%d,%d,0,%s,%s;isp:%d;bgsem:0;num:%ld;bulb:%s;",
             PANO_STEP_START, p->h_num, p->v_num, p->start_dir,
             ha, va, p->isp ? 1 : 0, num, bl);
}

int prog_pano_preview(const pano_params_t *p, char *out, size_t outcap) {
    char args[PLINK_ARGS_MAX];
    if (!p) { if (outcap) out[0] = 0; return 0; }
    pano_start_frame(p, args, sizeof args);
    return snprintf(out, outcap, "1&%d&2&%s#", PROG_CMD_PANO, args);
}

/* ----------------------------------------------------------------- start */

static void begin(prog_kind_t kind, long total, const char *frame) {
    memset(&g, 0, sizeof g);
    g.kind = kind;
    g.started_ms = plink_now_ms();
    g.last_reply_ms = g.started_ms;
    g.next_poll_ms = g.started_ms + POLL_MS;
    g.total = total;
    g.remaining = (total == -1) ? -2 : -1;
    snprintf(g.start_frame, sizeof g.start_frame, "%s", frame ? frame : "");
}

int prog_start_lapse(const lapse_params_t *p, char *err, size_t errcap) {
    char point[PLINK_ARGS_MAX], end[PLINK_ARGS_MAX];
    if (prog_check_lapse(p, err, errcap) != 0) return -1;
    if (plink_status() != PLINK_UP) return fail(err, errcap, "control link is down");
    if (g.kind != PROG_NONE) return fail(err, errcap, "a programme is already running");

    lapse_point_frame(p, point, sizeof point);
    lapse_end_frame(p, end, sizeof end);

    /* START, then the single waypoint, then SEND_END. The order is the app's:
     * the head collects points between 1 and 3 and begins on 3. */
    if (plink_send(PROG_CMD_LAPSE, 2, "step:1;") != 0)
        return fail(err, errcap, "could not send the start step");
    plink_send(PROG_CMD_LAPSE, 2, point);
    plink_send(PROG_CMD_LAPSE, 2, end);

    begin(PROG_LAPSE, p->shots, end);
    g.lapse = *p;
    fprintf(stderr, "[prog] timelapse start: %s | %s\n", point, end);
    return 0;
}

int prog_start_pano(const pano_params_t *p, char *err, size_t errcap) {
    char args[PLINK_ARGS_MAX], frame[PLINK_ARGS_MAX];
    if (prog_check_pano(p, err, errcap) != 0) return -1;
    if (plink_status() != PLINK_UP) return fail(err, errcap, "control link is down");
    if (g.kind != PROG_NONE) return fail(err, errcap, "a programme is already running");

    pano_start_frame(p, args, sizeof args);
    prog_pano_preview(p, frame, sizeof frame);
    /* Logged BEFORE sending. The start payload is inference; if the head does
     * something unexpected, the exact bytes that caused it are already on
     * disk rather than needing to be reconstructed afterwards. */
    fprintf(stderr, "[prog] panorama start (payload is INFERRED, see docs): %s\n", frame);

    if (plink_send(PROG_CMD_PANO, 2, args) != 0)
        return fail(err, errcap, "could not send the start step");

    begin(PROG_PANO, (long)p->h_num * p->v_num * p->per_spot, frame);
    g.pano = *p;
    /* The app sends the per-spot interval as its own live command; the start
     * order does not carry it. */
    if (p->interval_s > 0) prog_pano_interval(p->interval_s);
    return 0;
}

/* ---------------------------------------------------------------- control */

void prog_cancel(void) {
    if (g.kind == PROG_LAPSE) plink_send(PROG_CMD_LAPSE, 2, "step:7;");
    if (g.kind == PROG_PANO)  plink_send(PROG_CMD_PANO,  2, "step:6;");
    memset(&g, 0, sizeof g);
}

int prog_pano_pause(int paused) {
    char args[64];
    if (g.kind != PROG_PANO) return -1;
    snprintf(args, sizeof args, "step:%d;pause:%d;", PANO_STEP_PAUSE, paused ? 1 : 0);
    if (plink_send(PROG_CMD_PANO, 2, args) != 0) return -1;
    g.paused = paused ? 1 : 0;
    return 0;
}

int prog_pano_interval(int seconds) {
    char args[64];
    if (g.kind != PROG_PANO) return -1;
    if (seconds < 0) seconds = 0;
    if (seconds > 20) seconds = 20;
    snprintf(args, sizeof args, "step:%d;interval:%d;", PANO_STEP_INTERVAL, seconds);
    g.pano.interval_s = seconds;
    return plink_send(PROG_CMD_PANO, 2, args);
}

/* ------------------------------------------------------------------ poll */

/* Both programmes report progress the same way: ask for the remaining count,
 * the head answers, ask again. The reply carries it in `ret:`, falling back to
 * `num:` (POC:1836-1848). */
static void absorb_reply(void) {
    const plink_slot_t *s = plink_get(g.kind == PROG_PANO ? PROG_CMD_PANO : PROG_CMD_LAPSE);
    static unsigned long seen_pano = 0, seen_lapse = 0;
    unsigned long *seen = (g.kind == PROG_PANO) ? &seen_pano : &seen_lapse;
    double v;
    if (!s || s->count == *seen) return;
    *seen = s->count;
    g.last_reply_ms = plink_now_ms();
    g.stale = 0;

    v = plink_arg_num(s->args, "ret", NAN);
    if (!(v == v)) v = plink_arg_num(s->args, "num", NAN);
    if (v == v && v >= 0) {
        g.remaining = (long)v;
        /* Zero remaining is the head saying it is done. */
        if (g.remaining == 0) {
            fprintf(stderr, "[prog] %s complete\n",
                    g.kind == PROG_PANO ? "panorama" : "timelapse");
            memset(&g, 0, sizeof g);
        }
    }
}

void prog_tick(void) {
    double now;
    if (g.kind == PROG_NONE) return;

    /* A programme cannot outlive the link: without it we can neither observe
     * nor stop the run, and claiming progress we cannot see would be a lie. */
    if (plink_status() != PLINK_UP) {
        fprintf(stderr, "[prog] link lost while a programme was running — "
                        "the head may still be shooting\n");
        memset(&g, 0, sizeof g);
        return;
    }

    absorb_reply();
    if (g.kind == PROG_NONE) return;

    now = plink_now_ms();
    if (now >= g.next_poll_ms) {
        g.next_poll_ms = now + POLL_MS;
        plink_send(g.kind == PROG_PANO ? PROG_CMD_PANO : PROG_CMD_LAPSE, 2,
                   g.kind == PROG_PANO ? "step:3;" : "step:4;");
    }
    /* Stale, not finished. See STALE_MS. */
    if (!g.stale && now - g.last_reply_ms > STALE_MS) {
        g.stale = 1;
        fprintf(stderr, "[prog] no progress reply for %.0f s — run state unknown\n",
                STALE_MS / 1000.0);
    }
}

double prog_next_deadline_ms(void) {
    double d;
    if (g.kind == PROG_NONE) return -1;
    d = g.next_poll_ms - plink_now_ms();
    return d < 0 ? 0 : d;
}

prog_kind_t prog_running(void) { return g.kind; }

int lapse_durations(const lapse_params_t *p, double *shooting_s, double *video_s) {
    if (!p || p->shots < 1) { if (shooting_s) *shooting_s = 0; if (video_s) *video_s = 0; return 0; }
    if (shooting_s) *shooting_s = (p->shots - 1) * p->interval_s;
    if (video_s)    *video_s    = ceil((double)p->shots / (p->fps > 0 ? p->fps : 24));
    return 1;
}

int prog_status_json(char *out, int cap) {
    int n = 0;
    const char *kind = g.kind == PROG_LAPSE ? "timelapse"
                     : g.kind == PROG_PANO  ? "panorama" : "none";
    n += snprintf(out + n, cap - n, "{\"kind\":\"%s\"", kind);
    if (g.kind == PROG_NONE) return n + snprintf(out + n, cap - n, "}");

    n += snprintf(out + n, cap - n,
        ",\"elapsed_s\":%.0f,\"total\":%ld,\"remaining\":%ld,\"unlimited\":%s,"
        "\"paused\":%s,\"stale\":%s",
        (plink_now_ms() - g.started_ms) / 1000.0,
        g.total, g.remaining,
        g.total == -1 ? "true" : "false",
        g.paused ? "true" : "false",
        g.stale ? "true" : "false");
    if (g.total > 0 && g.remaining >= 0)
        n += snprintf(out + n, cap - n, ",\"taken\":%ld", g.total - g.remaining);
    if (g.kind == PROG_PANO)
        n += snprintf(out + n, cap - n,
            ",\"cols\":%d,\"rows\":%d,\"per_spot\":%d,\"interval_s\":%d",
            g.pano.h_num, g.pano.v_num, g.pano.per_spot, g.pano.interval_s);
    if (g.kind == PROG_LAPSE)
        n += snprintf(out + n, cap - n, ",\"interval_s\":%.3f,\"fps\":%d",
                      g.lapse.interval_s, g.lapse.fps);
    return n + snprintf(out + n, cap - n, "}");
}
