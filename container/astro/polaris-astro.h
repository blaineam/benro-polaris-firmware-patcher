/* SPDX-License-Identifier: MIT
 *
 * polaris-astro — celestial mode: enter/leave, compass alignment, tracking.
 *
 * WHY THE SERVER OWNS AN "ASTRO SESSION"
 * -------------------------------------
 * Two head-side things have to be kept alive for as long as you are doing
 * astro, and a browser cannot be trusted to keep either beating:
 *
 *   * The attitude push is a HEARTBEAT. `SP_SET_AHRS_STATE(1)` (520) has to be
 *     re-sent about every 5 s or the quaternion stream stops and every later
 *     GOTO computes against a stale frame (docs/APP-FEATURES.md, ASTRO pitfall
 *     2). So the server re-arms it on a timer while the session is open, and
 *     sends `state:0` when it closes -- leaving AHRS on outside astro/sun is
 *     called out as a battery drain (protocol §, "AHRS is mode-scoped").
 *
 *   * The mode itself. Entering astro is `SP_SET_MODE_STATE(8)` (285); the head
 *     will not track until it is in mode 8, and you cannot leave mode 8 while
 *     tracking (protocol §). The session models that: leaving stops tracking
 *     first.
 *
 * COMPASS ALIGNMENT WITHOUT A PHONE
 * --------------------------------
 * The phone app aligns by sending its own magnetometer heading:
 * `SP_SET_YAW(phoneDegrees, lat, lng)` (527). A browser has no reliable absolute
 * compass, so this offers the two honest substitutes the features doc points to:
 *
 *   1. Type the bearing. `astro_align(deg)` sends 527 with the heading the
 *      operator read off a real compass (or a phone compass app) -- the app's
 *      own instruction is to hold the phone AGAINST the head, so the phone was
 *      only ever a bearing proxy; any bearing source is equally valid.
 *   2. Plate-solve. The solver on /legacy already writes 527 from an actual
 *      solve and is strictly more accurate than any compass; this module just
 *      exposes the manual path and points at the solver for the precise one.
 *
 * 527 is PERSISTENT and its `ret` polarity is documented as contradictory, so
 * alignment is confirmed by the mount's own reported state, never by 527's ret.
 */
#ifndef POLARIS_ASTRO_H
#define POLARIS_ASTRO_H

#include <stddef.h>

#define ASTRO_MODE          8      /* SP_SET_MODE_STATE(8)                    */
#define ASTRO_CMD_MODE    285
#define ASTRO_CMD_AHRS    520
#define ASTRO_CMD_TRACK   531
#define ASTRO_CMD_YAW     527
#define ASTRO_CMD_HALF    536

/* Tracking rate body (the `speed:` field of 531): 0 sidereal (stars), 2 lunar.
 * The app sets this implicitly from the target; here it is explicit because
 * there is no star catalogue to infer it from. Rate 1 is unconfirmed and not
 * offered. */
#define ASTRO_RATE_SIDEREAL 0
#define ASTRO_RATE_LUNAR    2

/* Open the astro session: enter mode 8 and start the AHRS heartbeat. The
 * server's own lat/lon (from --lat/--lon) are used for alignment. Idempotent. */
int  astro_enter(double lat, double lon, char *err, size_t errcap);

/* Leave: stop tracking, stop the AHRS heartbeat, and (unless keep_mode) return
 * the head to plain photo mode. Safe to call when not in a session. */
void astro_leave(void);

int  astro_active(void);

/* Set the compass heading (527), in degrees 0..360. The session's lat/lon ride
 * along. This is the browser's stand-in for the phone-compass alignment. */
int  astro_align(double compass_deg, char *err, size_t errcap);

/* Start/stop tracking (531). `half` selects half-rate (536, which the wire
 * spells `halfSpeed:1` -- the app double-inverts it). Requires the head to be
 * aligned and, for actual motion, the Astro Kit fitted; the head refuses
 * otherwise and the caller surfaces that. */
int  astro_track(int on, int rate, int half, char *err, size_t errcap);

/* Re-arm the AHRS heartbeat if due. Call from the event loop. */
void astro_tick(void);
double astro_next_deadline_ms(void);

/* Machine-readable state for the UI: whether the session is open, the last
 * alignment heading sent, and the tracking request. The authoritative
 * aligned/tracking truth comes from the mount's own 284 telemetry, which the
 * UI reads separately -- this reports only what the session asked for. */
int  astro_status_json(char *out, int cap);

#endif /* POLARIS_ASTRO_H */
