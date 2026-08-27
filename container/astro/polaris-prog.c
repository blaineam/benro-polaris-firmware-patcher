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
    focus_params_t focus;
    hdr_params_t   hdr;
} state_t;

static state_t g;

/* The opcode a running programme reports on, and the poll it answers.
 *   poll_step = 0 -> the head PUSHES progress; we send nothing (HDR).
 * The remaining count is read from `rem_key`, falling back to `rem_key2`.
 * Completion is signalled either by remaining hitting 0, or by the head
 * pushing `done_step` on its own (HDR step 3). */
typedef struct {
    int         cmd;
    const char *poll_step;   /* what we send to ask; NULL/"" = do not poll  */
    const char *rem_key;     /* primary remaining-count key in the reply    */
    const char *rem_key2;    /* fallback key                                */
    const char *done_step;   /* a pushed step value that means "finished"   */
} prog_wire_t;

static prog_wire_t wire_for(prog_kind_t k) {
    switch (k) {
        /* Timelapse and panorama answer a poll with the count in `ret`
         * (panorama falls back to `num`). */
        case PROG_LAPSE: return (prog_wire_t){ PROG_CMD_LAPSE, "step:4;", "ret", NULL, NULL };
        case PROG_PANO:  return (prog_wire_t){ PROG_CMD_PANO,  "step:3;", "ret", "num", NULL };
        /* Focus stack answers RUNNING_INFO with `remainNum`. */
        case PROG_FOCUS: return (prog_wire_t){ PROG_CMD_FOCUS, "step:7;", "remainNum", NULL, "5" };
        /* HDR is PUSH-driven: the head sends step:5;ret:<remaining> and step:3
         * on completion, so we poll nothing. */
        case PROG_HDR:   return (prog_wire_t){ PROG_CMD_HDR, "", "ret", NULL, "3" };
        /* FREE PROGRAM polls RUNTIME (step 5); its ret is the appointment state
         * (0 ended, -1 failed, 2 started), not a frame count, so there is no
         * remaining number to read -- completion is the pushed/answered 0. */
        case PROG_PLC:   return (prog_wire_t){ PROG_CMD_PLC, "step:5;", "ret", NULL, "0" };
        default:         return (prog_wire_t){ 0, "", "", NULL, NULL };
    }
}

/* FREE PROGRAM keyframes live here between "add key" calls and "start", so the
 * user can fly the head, capture a pose, fly again, capture again. Separate
 * from the running state (`g`) because they are built BEFORE a run. */
static plc_key_t  g_keys[PLC_MAX_KEYS];
static int        g_nkeys;

/* ------------------------------------------------------------------ util */

static int fail(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
    return -1;
}

/* The head wants plain decimals. "%g" would emit 1e+06 for a long unlimited
 * run's derived values and the parser on the other side is a substring scan. */
static void fmt_num(char *out, size_t cap, double v) {
    if (v == floor(v) && fabs(v) < 1e9) { snprintf(out, cap, "%ld", (long)v); return; }
    /* Enough precision for a radian pose (~5 decimals ≈ 0.0006°) without the
     * trailing-zero noise of a fixed %f -- the head does Float.parseFloat, so a
     * minimal decimal is both smaller on the wire and easier to read in a log. */
    snprintf(out, cap, "%.5f", v);
    { char *e = out + strlen(out) - 1;
      while (e > out && *e == '0') *e-- = 0;
      if (e > out && *e == '.') *e = 0; }
}

/* Read the camera's live selection for one exposure axis out of the link
 * cache. The option-list replies (265/266/267/268/275) carry
 * `RD:<avail>;V:<index>;R:<comma,list>;` -- V is the selected INDEX, which is
 * exactly what the set commands and the HDR frame want. Returns the index, or
 * `dflt` when the camera has not reported that axis yet. `count` (optional)
 * receives the number of options, so a shifted index can be clamped to the
 * real list rather than guessed. */
static int cam_index(int get_cmd, int *count, int dflt) {
    const plink_slot_t *s = plink_get(get_cmd);
    int idx;
    if (count) *count = 0;
    if (!s) return dflt;
    idx = (int)plink_arg_num(s->args, "V", dflt);
    if (count) {
        char list[PLINK_ARGS_MAX];
        if (plink_arg(s->args, "R", list, sizeof list) == 0 && list[0]) {
            int n = 1; const char *p;
            for (p = list; *p; p++) if (*p == ',') n++;
            *count = n;
        }
    }
    return idx;
}

/* The nth option's human label from an axis's cached R: list, or "?" if the
 * camera has not reported it. Used to show the three real shutter speeds an HDR
 * bracket will use, so the frame is never a mystery before it fires. */
static void cam_label(int get_cmd, int index, char *out, size_t cap) {
    const plink_slot_t *s = plink_get(get_cmd);
    char list[PLINK_ARGS_MAX];
    int i = 0; char *p, *save = NULL;
    snprintf(out, cap, "?");
    if (!s || plink_arg(s->args, "R", list, sizeof list) != 0) return;
    for (p = strtok_r(list, ",", &save); p; p = strtok_r(NULL, ",", &save), i++)
        if (i == index) { snprintf(out, cap, "%s", p); return; }
}

/* ---------------------------------------------------------- validation */

int prog_check_hdr(const hdr_params_t *p, char *err, size_t errcap) {
    if (!p) return fail(err, errcap, "no parameters");
    if (p->spread < 1 || p->spread > 30)
        return fail(err, errcap, "bracket spread must be 1..30 shutter steps");
    if (plink_status() != PLINK_UP) return fail(err, errcap, "control link is down");
    /* HDR needs the camera's shutter list to build the three frames; without it
     * we would be inventing indices. Refusing beats bracketing blind. */
    if (!plink_get(268))
        return fail(err, errcap, "the camera's shutter list has not loaded yet — "
                                 "open the Control tab once so it can be read");
    return 0;
}

int prog_check_focus(const focus_params_t *p, char *err, size_t errcap) {
    if (!p) return fail(err, errcap, "no parameters");
    if (p->shots < 2 || p->shots > 200)
        return fail(err, errcap, "shots must be between 2 and 200");
    return 0;
}

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

/* 280 step 1 (HDRLayout.getStartShootingParameter):
 *     step:1;isp:<0|1>;p1:<bulb>,<sIdx>,<fIdx>,<evIdx>,<isoIdx>,<wbIdx>;p2:…;p3:…;
 * p1=under, p2=normal, p3=over. Every axis is an INDEX into the camera's own
 * option list. We hold aperture/EV/ISO/WB at the current setting and bracket
 * only the shutter, `spread` list-entries either side of the current index --
 * which is what "one stop, quantised to what the camera offers" reduces to when
 * the head speaks indices. `shut_idx` receives the three indices so the caller
 * can show the real shutter labels. */
static void hdr_frames(const hdr_params_t *p, int shut_idx[3], char *out, size_t cap) {
    int scount = 0;
    int sV = cam_index(268, &scount, 0);
    int fV = cam_index(275, NULL, 0);
    int eV = cam_index(267, NULL, 0);
    int iV = cam_index(265, NULL, 0);
    int wV = cam_index(266, NULL, 0);
    int lo = sV - p->spread, hi = sV + p->spread;
    int order[3];
    /* Clamp to the real list; if the list length is unknown, only clamp the
     * low end (a negative index is always wrong, a high one merely maybe). */
    if (lo < 0) lo = 0;
    if (scount > 0 && hi > scount - 1) hi = scount - 1;
    /* p1 under / p2 normal / p3 over. Which shutter index is "under" (darker)
     * depends on the list's direction, which is not established -- so this does
     * NOT assert a direction: it emits the three indices lo,mid,hi in a fixed
     * order and lets the operator read the resulting speeds in the preview. */
    order[0] = lo; order[1] = sV; order[2] = hi;
    shut_idx[0] = order[0]; shut_idx[1] = order[1]; shut_idx[2] = order[2];
    snprintf(out, cap,
        "step:%d;isp:%d;p1:0,%d,%d,%d,%d,%d;p2:0,%d,%d,%d,%d,%d;p3:0,%d,%d,%d,%d,%d;",
        HDR_STEP_START, p->isp ? 1 : 0,
        order[0], fV, eV, iV, wV,
        order[1], fV, eV, iV, wV,
        order[2], fV, eV, iV, wV);
}

int prog_hdr_preview(const hdr_params_t *p, char *out, size_t outcap) {
    char args[PLINK_ARGS_MAX];
    int idx[3];
    char a[24], b[24], c[24];
    if (!p) { if (outcap) out[0] = 0; return 0; }
    hdr_frames(p, idx, args, sizeof args);
    cam_label(268, idx[0], a, sizeof a);
    cam_label(268, idx[1], b, sizeof b);
    cam_label(268, idx[2], c, sizeof c);
    /* The frame, then the three human shutter speeds it resolves to. */
    return snprintf(out, outcap, "1&%d&2&%s#\nshutters: %s / %s / %s",
                    PROG_CMD_HDR, args, a, b, c);
}

/* 270 step 3 (SP_FOCUS_STACK_START + FocusTrackLayout.getStartShootingParameter):
 *     step:3;;num:<shots>;isp:<0|1>;
 * The double semicolon is real -- getStartShootingParameter returns ";num:…;"
 * and the caller prepends "step:3;". Same substring-protocol reasoning as the
 * timelapse step-3 frame: do not tidy it. */
static void focus_start_frame(const focus_params_t *p, int step, char *out, size_t cap) {
    snprintf(out, cap, "step:%d;;num:%d;isp:%d;", step, p->shots, p->isp ? 1 : 0);
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

int prog_start_hdr(const hdr_params_t *p, char *err, size_t errcap) {
    char args[PLINK_ARGS_MAX], frame[PLINK_ARGS_MAX];
    int idx[3];
    if (prog_check_hdr(p, err, errcap) != 0) return -1;
    if (g.kind != PROG_NONE) return fail(err, errcap, "a programme is already running");

    hdr_frames(p, idx, args, sizeof args);
    prog_hdr_preview(p, frame, sizeof frame);
    fprintf(stderr, "[prog] HDR start: %s\n", frame);
    if (plink_send(PROG_CMD_HDR, 2, args) != 0)
        return fail(err, errcap, "could not send the start step");

    begin(PROG_HDR, 3, args);   /* always exactly three frames */
    g.hdr = *p;
    return 0;
}

int prog_focus_mark(int which) {
    /* Marking a limit is meaningful any time before a run; it is not gated on a
     * running programme, since it is what you do to SET one up. */
    if (plink_status() != PLINK_UP) return -1;
    return plink_send(PROG_CMD_FOCUS, 2,
                      which ? "step:2;" : "step:1;");
}

int prog_focus_preview(const focus_params_t *p, char *err, size_t errcap) {
    char args[PLINK_ARGS_MAX];
    if (prog_check_focus(p, err, errcap) != 0) return -1;
    if (plink_status() != PLINK_UP) return fail(err, errcap, "control link is down");
    if (g.kind != PROG_NONE) return fail(err, errcap, "a programme is already running");
    focus_start_frame(p, FOCUS_STEP_PREVIEW, args, sizeof args);
    if (plink_send(PROG_CMD_FOCUS, 2, args) != 0)
        return fail(err, errcap, "could not send the preview step");
    /* The preview is a dry traverse; treat it as a running programme so its
     * step counter is polled and it can be cancelled, but mark it a preview so
     * the UI can say so. Reuses the focus poll (step 7). */
    begin(PROG_FOCUS, p->shots, args);
    g.focus = *p;
    g.paused = 2;   /* sentinel: "preview", surfaced in the status JSON */
    return 0;
}

int prog_start_focus(const focus_params_t *p, char *err, size_t errcap) {
    char args[PLINK_ARGS_MAX];
    if (prog_check_focus(p, err, errcap) != 0) return -1;
    if (plink_status() != PLINK_UP) return fail(err, errcap, "control link is down");
    if (g.kind != PROG_NONE) return fail(err, errcap, "a programme is already running");
    focus_start_frame(p, FOCUS_STEP_START, args, sizeof args);
    fprintf(stderr, "[prog] focus-stack start: %s\n", args);
    if (plink_send(PROG_CMD_FOCUS, 2, args) != 0)
        return fail(err, errcap, "could not send the start step");
    begin(PROG_FOCUS, p->shots, args);
    g.focus = *p;
    return 0;
}

/* --------------------------------------------------------- free program */

void prog_plc_clear(void) { g_nkeys = 0; }
int  prog_plc_key_count(void) { return g_nkeys; }

/* Copy the captured keyframes into a caller buffer for a start/preview. Returns
 * the count actually copied. Kept separate from the params struct so the HTTP
 * layer, which assembles the rest of the params from the request, does not have
 * to know how the keys are stored. */
int prog_plc_key_count_export(plc_key_t *out, int cap) {
    int i, n = g_nkeys < cap ? g_nkeys : cap;
    for (i = 0; i < n; i++) out[i] = g_keys[i];
    return n;
}

int prog_plc_add_key(double t_s, char *err, size_t errcap) {
    const plink_slot_t *s = plink_get(517);   /* current pose, radians */
    plc_key_t *k;
    if (g_nkeys >= PLC_MAX_KEYS) return fail(err, errcap, "no room for more keyframes");
    if (!s) return fail(err, errcap, "the head has not reported its pose yet");
    if (t_s < 0) return fail(err, errcap, "time cannot be negative");
    k = &g_keys[g_nkeys];
    k->t_s = t_s;
    /* 517 reports yaw/pitch/roll in radians; store them verbatim -- the whole
     * point of capturing live is that no frame conversion is needed. */
    k->pan  = plink_arg_num(s->args, "yaw", 0);
    k->tilt = plink_arg_num(s->args, "pitch", 0);
    k->roll = plink_arg_num(s->args, "roll", 0);
    g_nkeys++;
    return g_nkeys;
}

int prog_check_plc(const plc_params_t *p, char *err, size_t errcap) {
    int i;
    if (!p) return fail(err, errcap, "no parameters");
    if (p->n_keys < 2) return fail(err, errcap, "a programme needs at least two motion keyframes");
    if (p->n_keys > PLC_MAX_KEYS) return fail(err, errcap, "too many keyframes");
    /* Times must strictly increase: two keys at the same instant have no
     * interpolation between them and a decreasing time reverses the timeline. */
    for (i = 1; i < p->n_keys; i++)
        if (!(p->keys[i].t_s > p->keys[i-1].t_s))
            return fail(err, errcap, "keyframe times must increase");
    if (p->photo_interval_s < 0 || p->photo_interval_s > 3600)
        return fail(err, errcap, "photo interval must be 0..3600 seconds");
    if (p->photo_count != -1 && (p->photo_count < 0 || p->photo_count > 100000))
        return fail(err, errcap, "photo count out of range");
    return 0;
}

/* Build the timeline frames without sending. Returns bytes written; the frames
 * are newline-separated so the preview reads as the upload it is. */
int prog_plc_preview(const plc_params_t *p, char *out, size_t outcap) {
    int i, n = 0;
    char pan[24], tilt[24], roll[24], tm[24];
    double last_t = 0;
    if (!p) { if (outcap) out[0] = 0; return 0; }
    n += snprintf(out + n, outcap - n, "1&%d&2&step:1;#", PROG_CMD_PLC);
    /* Photo track (item 1), if any photos are wanted. */
    if (p->photo_interval_s > 0) {
        fmt_num(tm, sizeof tm, p->photo_interval_s);
        n += snprintf(out + n, outcap - n,
            "\n1&%d&2&step:2;item:1;point:1;time:0;para:%ld,%s;#",
            PROG_CMD_PLC, p->photo_count, tm);
    }
    /* Motion track (item 3), one frame per keyframe. mode:0 linear / mode:1
     * hold -- the app's isLineModel==false is the linear case. */
    for (i = 0; i < p->n_keys; i++) {
        fmt_num(pan,  sizeof pan,  p->keys[i].pan);
        fmt_num(tilt, sizeof tilt, p->keys[i].tilt);
        fmt_num(roll, sizeof roll, p->keys[i].roll);
        fmt_num(tm,   sizeof tm,   p->keys[i].t_s);
        n += snprintf(out + n, outcap - n,
            "\n1&%d&2&step:2;item:3;point:%d;time:%s;para:%s,%s,%s;mode:%d;#",
            PROG_CMD_PLC, i + 1, tm, pan, tilt, roll, p->hold ? 1 : 0);
        last_t = p->keys[i].t_s;
    }
    /* END_POINT carries the per-track counts and the last time. item2 (camera
     * params) is unused here, so its count is 0. */
    fmt_num(tm, sizeof tm, last_t);
    n += snprintf(out + n, outcap - n,
        "\n1&%d&2&step:3;item1:%d,%s;item2:0,%s;item3:%d,%s;#",
        PROG_CMD_PLC,
        p->photo_interval_s > 0 ? 1 : 0, tm,
        tm, p->n_keys, tm);
    return n;
}

int prog_start_plc(const plc_params_t *p, char *err, size_t errcap) {
    int i;
    char frame[PLINK_ARGS_MAX], tm[24], pan[24], tilt[24], roll[24];
    double last_t;
    if (prog_check_plc(p, err, errcap) != 0) return -1;
    if (plink_status() != PLINK_UP) return fail(err, errcap, "control link is down");
    if (g.kind != PROG_NONE) return fail(err, errcap, "a programme is already running");

    plink_send(PROG_CMD_PLC, 2, "step:1;");
    if (p->photo_interval_s > 0) {
        fmt_num(tm, sizeof tm, p->photo_interval_s);
        snprintf(frame, sizeof frame, "step:2;item:1;point:1;time:0;para:%ld,%s;",
                 p->photo_count, tm);
        plink_send(PROG_CMD_PLC, 2, frame);
    }
    for (i = 0; i < p->n_keys; i++) {
        fmt_num(pan,  sizeof pan,  p->keys[i].pan);
        fmt_num(tilt, sizeof tilt, p->keys[i].tilt);
        fmt_num(roll, sizeof roll, p->keys[i].roll);
        fmt_num(tm,   sizeof tm,   p->keys[i].t_s);
        snprintf(frame, sizeof frame,
                 "step:2;item:3;point:%d;time:%s;para:%s,%s,%s;mode:%d;",
                 i + 1, tm, pan, tilt, roll, p->hold ? 1 : 0);
        plink_send(PROG_CMD_PLC, 2, frame);
    }
    last_t = p->keys[p->n_keys - 1].t_s;
    fmt_num(tm, sizeof tm, last_t);
    snprintf(frame, sizeof frame, "step:3;item1:%d,%s;item2:0,%s;item3:%d,%s;",
             p->photo_interval_s > 0 ? 1 : 0, tm, tm, p->n_keys, tm);
    plink_send(PROG_CMD_PLC, 2, frame);

    /* A scheduled start rides step 4; otherwise it begins on END_POINT. */
    if (p->appointment_unix > 0) {
        snprintf(frame, sizeof frame, "step:4;time:%ld;", p->appointment_unix);
        plink_send(PROG_CMD_PLC, 2, frame);
    }

    fprintf(stderr, "[prog] free-program start: %d keys over %.1f s%s\n",
            p->n_keys, last_t, p->appointment_unix ? " (scheduled)" : "");
    /* No frame count -- progress is the appointment state, so total is unknown. */
    begin(PROG_PLC, -1, "free-program");
    g.remaining = -1;
    return 0;
}

/* ---------------------------------------------------------------- control */

void prog_cancel(void) {
    switch (g.kind) {
        case PROG_LAPSE: plink_send(PROG_CMD_LAPSE, 2, "step:7;"); break;
        case PROG_PANO:  plink_send(PROG_CMD_PANO,  2, "step:6;"); break;
        case PROG_HDR:   plink_send(PROG_CMD_HDR,   2, "step:4;"); break;
        /* Focus: cancel the run, or the preview (step 10) if that is what is
         * live -- the sentinel in `paused` tells them apart. */
        case PROG_FOCUS: plink_send(PROG_CMD_FOCUS, 2, g.paused == 2 ? "step:10;" : "step:6;"); break;
        case PROG_PLC:   plink_send(PROG_CMD_PLC,   2, "step:6;"); break;
        default: break;
    }
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
    prog_wire_t w = wire_for(g.kind);
    const plink_slot_t *s = plink_get(w.cmd);
    static unsigned long seen[PROG_PLC + 1];
    double v;
    char step[8] = "";
    if (!s || s->count == seen[g.kind]) return;
    seen[g.kind] = s->count;
    g.last_reply_ms = plink_now_ms();
    g.stale = 0;

    /* A pushed completion step ends the run regardless of any count -- HDR
     * signals done this way (step:3), and focus's COMPLETION (step:5) does too. */
    if (w.done_step && plink_arg(s->args, "step", step, sizeof step) == 0 &&
        !strcmp(step, w.done_step)) {
        fprintf(stderr, "[prog] %s complete (step %s)\n",
                g.kind == PROG_HDR ? "HDR" : "focus stack", step);
        memset(&g, 0, sizeof g);
        return;
    }

    v = plink_arg_num(s->args, w.rem_key, NAN);
    if (!(v == v) && w.rem_key2) v = plink_arg_num(s->args, w.rem_key2, NAN);
    if (v == v && v >= 0) {
        g.remaining = (long)v;
        if (g.remaining == 0) {
            fprintf(stderr, "[prog] programme complete\n");
            memset(&g, 0, sizeof g);
        }
    }
}

void prog_tick(void) {
    double now;
    prog_wire_t w;
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
    w = wire_for(g.kind);
    /* HDR is push-only (poll_step ""), so it is never polled -- it just waits
     * for the head's step:5/step:3. Everything else asks on the POLL_MS cadence. */
    if (w.poll_step && w.poll_step[0] && now >= g.next_poll_ms) {
        g.next_poll_ms = now + POLL_MS;
        plink_send(w.cmd, 2, w.poll_step);
    }
    /* Stale, not finished. See STALE_MS. For HDR, "stale" only means the head
     * has gone quiet -- expected between its short bracket and completion, so
     * the UI treats HDR staleness gently. */
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
                     : g.kind == PROG_PANO  ? "panorama"
                     : g.kind == PROG_HDR   ? "hdr"
                     : g.kind == PROG_FOCUS ? "focus"
                     : g.kind == PROG_PLC   ? "program" : "none";
    n += snprintf(out + n, cap - n, "{\"kind\":\"%s\"", kind);
    if (g.kind == PROG_NONE) return n + snprintf(out + n, cap - n, "}");

    n += snprintf(out + n, cap - n,
        ",\"elapsed_s\":%.0f,\"total\":%ld,\"remaining\":%ld,\"unlimited\":%s,"
        "\"paused\":%s,\"preview\":%s,\"stale\":%s",
        (plink_now_ms() - g.started_ms) / 1000.0,
        g.total, g.remaining,
        g.total == -1 ? "true" : "false",
        /* paused==2 is the focus-PREVIEW sentinel, not a real pause. */
        (g.paused == 1) ? "true" : "false",
        (g.paused == 2) ? "true" : "false",
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
    if (g.kind == PROG_FOCUS)
        n += snprintf(out + n, cap - n, ",\"shots\":%d", g.focus.shots);
    return n + snprintf(out + n, cap - n, "}");
}
