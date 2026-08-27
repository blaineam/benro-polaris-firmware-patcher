/* SPDX-License-Identifier: MIT
 *
 * polaris-prog — the capture programmes: timelapse (272) and panorama (271).
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
 * The timelapse payloads are read directly from the app's own layout classes
 * and are solid. The PANORAMA START payload is NOT: `SP_PANORAMIC_START` has
 * zero call sites in the decompiled app and `MainActivity.clickStart` — the
 * method that binds its boolean arguments — failed to decompile
 * (docs/APP-FEATURES.md, "what a naive panorama port gets wrong", item 14). The
 * field ORDER and the units are established; the runtime binding of `isp`,
 * `bgsem` and `dir` is inference.
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
#define PROG_CMD_PANO       271
#define PROG_CMD_LAPSE      272

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
    PROG_PANO
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

/* Start a programme. Returns 0 on success, -1 with `err` filled otherwise
 * (link down, another programme already running, or a parameter out of range).
 * Both MOVE MOTORS; the HTTP layer gates them behind an explicit confirm. */
int prog_start_lapse(const lapse_params_t *p, char *err, size_t errcap);
int prog_start_pano (const pano_params_t  *p, char *err, size_t errcap);

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
