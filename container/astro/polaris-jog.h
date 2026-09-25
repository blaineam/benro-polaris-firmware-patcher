/* SPDX-License-Identifier: MIT
 *
 * polaris-jog — hold-to-move, with the dead man on OUR side of the wire.
 *
 * THE HAZARD
 * ----------
 * Two jog families exist (docs/APP-PROTOCOL.md §"Key-jog encoding"):
 *
 *   slow / latched   532 HADJ, 533 VADJ, 534 RADJ
 *                    key:<0|1>;state:<0|1>;level:<1..5>;
 *                    A state:1 press with NO matching state:0 leaves the axis
 *                    moving. Forever.
 *
 *   fast / continuous 513 HADJ, 514 VADJ, 521 RADJ
 *                    speed:<±100..2500>;
 *                    Must be re-sent every 50 ms to sustain motion. Whether the
 *                    head stops when the stream stops is NOT ESTABLISHED — the
 *                    alpaca driver deliberately skips stopping motors on
 *                    shutdown, so nobody has proven the head has a timeout.
 *
 * So the failure that matters is not "the UI sent a bad command". It is "the
 * browser went away while an axis was moving": a phone locking, wifi dropping
 * at the edge of the AP, a tab closing, the operator walking off. If the thing
 * responsible for sending the stop is the same thing that just disappeared,
 * nothing sends the stop.
 *
 * THE DESIGN
 * ----------
 * The browser never drives the wire. It declares an INTENT with a short
 * lease — "pan left at gear 3, and I will still be here in 400 ms" — and the
 * server owns the repeat and the stop:
 *
 *   * the 50 ms fast-jog repeat runs in the server's event loop, not in JS,
 *     so it is immune to a throttled background tab and costs no HTTP;
 *   * the lease is renewed by the UI while a button is held. If a renewal does
 *     not arrive before it expires, the server stops the axis. A dead browser
 *     therefore STOPS the mount instead of abandoning it in motion;
 *   * the link dropping stops every axis too, since a stop that cannot be sent
 *     must at least not be believed to have been sent.
 *
 * A stop is idempotent and cheap, so this errs toward sending too many.
 *
 * jog_tick() must be called from the event loop at least as often as the
 * repeat interval, and jog_next_deadline_ms() tells the loop how long it may
 * sleep, so an idle server still idles.
 */
#ifndef POLARIS_JOG_H
#define POLARIS_JOG_H

/* Physical axes, in the app's own naming. The motor mapping (M1/M2/M3) and the
 * warning that M1/M2 stop meaning azimuth/altitude once M3 has rotated both
 * live in docs/APP-PROTOCOL.md. */
typedef enum {
    JOG_PAN  = 0,     /* HADJ — 513 fast / 532 slow — M1 */
    JOG_TILT = 1,     /* VADJ — 514 fast / 533 slow — M2 */
    JOG_ROT  = 2,     /* RADJ — 521 fast / 534 slow — M3 */
    JOG_AXES = 3
} jog_axis_t;

/* How long an intent survives without renewal. Long enough that a renewal
 * every ~150 ms is comfortable over a busy 2.4 GHz AP, short enough that a
 * dead client cannot leave the head moving for more than a blink. */
#define JOG_LEASE_MS   400.0
/* Fast jog must be re-sent at this cadence to sustain motion (CTL:1015). */
#define JOG_REPEAT_MS  50.0
/* The head's own clip range for fast jog; magnitudes below ~100 do not move it
 * and above 2500 are clipped anyway. */
#define JOG_SPEED_MIN  100
#define JOG_SPEED_MAX  2500
/* Slow-jog gears. Level 0 is NOT a stop — it disengages torque (CTL:913). */
#define JOG_GEAR_MIN   1
#define JOG_GEAR_MAX   5

/* Start or renew motion on one axis.
 *
 *   speed  != 0 -> fast/continuous jog, magnitude clamped into
 *                  [JOG_SPEED_MIN, JOG_SPEED_MAX], sign = direction.
 *   speed  == 0 and gear != 0 -> slow/latched jog at that gear,
 *                  sign of `gear` = direction.
 *   both zero   -> equivalent to jog_stop().
 *
 * Renewing with identical parameters just extends the lease; the latched
 * family is deliberately NOT re-pressed, because it is latched and a second
 * press is not a no-op. Returns 0 on success, -1 if the link is down. */
int  jog_set(jog_axis_t axis, int speed, int gear);

/* Stop one axis now (releases a latched press, zeroes a continuous one). */
void jog_stop(jog_axis_t axis);

/* Stop everything. Called on link loss and at shutdown. */
void jog_stop_all(void);

/* Drive repeats and expire leases. Safe to call at any rate. */
void jog_tick(void);

/* Milliseconds until jog_tick() next has work, or -1 when nothing is moving
 * (so the event loop can sleep normally instead of spinning at 50 Hz). */
double jog_next_deadline_ms(void);

/* Is anything moving? Used to decide whether a stop needs sending on drop. */
int  jog_active(void);

/* Machine-readable state for the UI, appended to `out`. Returns bytes written. */
int  jog_status_json(char *out, int cap);

#endif /* POLARIS_JOG_H */
