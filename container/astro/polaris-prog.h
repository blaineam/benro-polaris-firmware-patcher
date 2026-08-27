/* SPDX-License-Identifier: MIT
 *
 * polaris-prog — the capture programmes: timelapse (272), panorama (271),
 *                HDR (280) and focus stacking (270).
 *
 * WHY THE SERVER OWNS THESE
 * -------------------------
 * Both are multi-step sequences the head runs on its own once started, and both
 * report progress by a poll the CLIENT is expected to sustain: the head answers
 * with the remaining count and the app immediately asks again
 * (PolarisOrderCommunication:2111-2129). Drop that and the counter freezes,
 * which is how the phone app behaves when it is backgrounded.
 *
 * Running the poll here instead buys the one genuinely new capability this port
 * has over the phone app: **a programme keeps running, and keeps being
 * observable, with no client connected at all**. Close the tab, walk away, come
 * back an hour later and the progress is still there. The phone app cannot do
 * that — closing it ends the session.
 *
 * THE HONEST CAVEAT, WHICH IS LOAD-BEARING
 * ----------------------------------------
 * The timelapse, HDR and focus-stack payloads are read directly from the app's
 * own `getStartShootingParameter()` builders and are solid — HDR's is the exact
 * `p1/p2/p3:<bulb>,<sIdx>,<fIdx>,<evIdx>,<isoIdx>,<wbIdx>` triple, focus's is
 * `;num:<shots>;`. The PANORAMA START payload is the one that is NOT solid:
 * `SP_PANORAMIC_START` has zero call sites and `MainActivity.clickStart` — which
 * binds its boolean arguments — failed to decompile (docs/APP-FEATURES.md,
 * "what a naive panorama port gets wrong", item 14). Its field ORDER and units
 * are established; the runtime binding of `isp`, `bgsem` and `dir` is inference.
 *
 * So panorama start is built here but never sent blind: prog_pano_preview()
 * renders the exact frame for a human to read first, the UI shows it, and the
 * frame is written to the log when it is sent. If it turns out to be wrong, the
 * evidence of what was actually sent is already in hand.
 */
#ifndef POLARIS_PROG_H
#define POLARIS_PROG_H

#include <stddef.h>

/* Opcodes and the step selectors, from docs/APP-PROTOCOL.md §5.6. */
#define PROG_CMD_FOCUS      270
#define PROG_CMD_PANO       271
#define PROG_CMD_LAPSE      272
#define PROG_CMD_HDR        280

/* Focus stack (270). Racking the lens between two limits is SETUP done before
 * the run: mark the near limit, rack, mark the far limit, then start. */
#define FOCUS_STEP_MARK_A    1
#define FOCUS_STEP_MARK_B    2
#define FOCUS_STEP_START     3
#define FOCUS_STEP_CANCEL    6
#define FOCUS_STEP_RUNNING   7
#define FOCUS_STEP_PREVIEW   8

/* HDR (280). Always exactly three frames; the head pushes progress
 * (step:5;ret:<remaining>) and completion (step:3) rather than being polled. */
#define HDR_STEP_START       1
#define HDR_STEP_CANCEL      4

#define PANO_STEP_START      2
#define PANO_STEP_REMAINING  3
#define PANO_STEP_END        4
#define PANO_STEP_COMPLETION 5
#define PANO_STEP_CANCEL     6
#define PANO_STEP_PAUSE     12
#define PANO_STEP_INTERVAL  13

#define LAPSE_STEP_START     1
#define LAPSE_STEP_POINT     2
#define LAPSE_STEP_SEND_END  3
#define LAPSE_STEP_REMAINING 4
#define LAPSE_STEP_CANCEL    7

typedef enum {
    PROG_NONE = 0,
    PROG_LAPSE,
    PROG_PANO,
    PROG_HDR,
    PROG_FOCUS
} prog_kind_t;

typedef struct {
    /* Interval between frames, seconds. The app offers a discrete list; any
     * positive value is accepted here and the UI offers the same list. */
    double interval_s;
    /* Total frames, or -1 for unlimited — which is the app's DEFAULT, and the
     * run then continues until stopped or the card fills. */
    long   shots;
    /* Bulb seconds, or 0 for the camera's own shutter setting. */
    double bulb_s;
    /* Frames per second of the finished video. Not sent to the head; used only
     * for the derived duration readout. */
    int    fps;
} lapse_params_t;

typedef struct {
    int    h_num;        /* columns                                         */
    int    v_num;        /* rows                                            */
    double h_angle;      /* DEGREES between adjacent columns                */
    double v_angle;      /* DEGREES between adjacent rows                   */
    int    start_dir;    /* 0 centre, 1 UL, 2 UR, 3 DL, 4 DR                */
    int    per_spot;     /* frames per position, 1..99                      */
    int    interval_s;   /* seconds between per-spot frames, 0..20          */
    double bulb_s;       /* bulb seconds, or 0                              */
    int    isp;          /* in-head stitching                               */
} pano_params_t;

typedef struct {
    /* Frames bracket the SHUTTER around the camera's current exposure, `spread`
     * entries apart in the camera's own shutter list (the head takes indices,
     * not stops, so this is unambiguous whatever the list spacing). The other
     * axes (aperture/EV/ISO/WB) are held at the current setting. */
    int spread;      /* shutter-list steps between adjacent frames, >=1        */
    int isp;         /* merge in the head                                      */
} hdr_params_t;

typedef struct {
    int shots;       /* 2..200                                                 */
    int isp;         /* stitch all-in-focus in the head                        */
} focus_params_t;

/* Start a programme. Returns 0 on success, -1 with `err` filled otherwise
 * (link down, another programme already running, or a parameter out of range).
 * Both MOVE MOTORS; the HTTP layer gates them behind an explicit confirm. */
int prog_start_lapse(const lapse_params_t *p, char *err, size_t errcap);
int prog_start_pano (const pano_params_t  *p, char *err, size_t errcap);
int prog_start_hdr  (const hdr_params_t   *p, char *err, size_t errcap);
int prog_start_focus(const focus_params_t *p, char *err, size_t errcap);

/* HDR and focus-stack render a preview too -- HDR because it resolves the three
 * shutter values from the camera's live indices (so you see the actual speeds
 * before firing), focus because a dry preview traverse exists in the protocol.
 * `hdr_preview` fills `shutters` with the three human labels when it can. */
int prog_hdr_preview  (const hdr_params_t *p, char *out, size_t outcap);
int prog_check_hdr    (const hdr_params_t *p, char *err, size_t errcap);
int prog_check_focus  (const focus_params_t *p, char *err, size_t errcap);

/* Focus-stack SETUP, before a run. Mark the near limit, rack the lens (via the
 * 311 focus-adjust opcode through the ordinary send path), mark the far limit.
 * `prog_focus_mark(0)` = near/start, `(1)` = far/end. */
int prog_focus_mark(int which);
/* A dry preview traverse between the marked limits (270 step 8). */
int prog_focus_preview(const focus_params_t *p, char *err, size_t errcap);

/* Render the exact frame a pano start WOULD send, without sending it. This
 * exists because the pano start payload is inference — see the header comment.
 * Returns bytes written. */
int prog_pano_preview(const pano_params_t *p, char *out, size_t outcap);

/* Validate without starting. Same checks prog_start_* apply. 0 = ok. */
int prog_check_lapse(const lapse_params_t *p, char *err, size_t errcap);
int prog_check_pano (const pano_params_t  *p, char *err, size_t errcap);

/* Stop whatever is running. Idempotent and always safe to call. */
void prog_cancel(void);

/* Panorama only: pause/resume, and change the per-spot interval mid-run (the
 * app sends interval changes live; the count only reaches the head at start). */
int prog_pano_pause(int paused);
int prog_pano_interval(int seconds);

/* Drive the progress poll and the completion watchdog. Call from the event
 * loop; cheap when nothing is running. */
void prog_tick(void);

/* Milliseconds until prog_tick() next has work, or -1 when idle. */
double prog_next_deadline_ms(void);

prog_kind_t prog_running(void);

/* Machine-readable state for the UI. Returns bytes written. */
int prog_status_json(char *out, int cap);

/* Derived timelapse figures, shared with the UI so both agree.
 *   shooting_s = (shots - 1) * interval   -- the GAPS, not the frames
 *   video_s    = ceil(shots / fps)
 * Both are LOWER BOUNDS: neither the app nor the head accounts for shutter or
 * the camera's processing time, so a real run always takes at least this long
 * and usually longer. Returns 0 if the run is unlimited. */
int lapse_durations(const lapse_params_t *p, double *shooting_s, double *video_s);

#endif /* POLARIS_PROG_H */
