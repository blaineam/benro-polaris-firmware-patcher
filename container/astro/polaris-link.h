/* SPDX-License-Identifier: MIT
 *
 * polaris-link — ONE long-lived, registered connection to the Polaris control
 * port, shared by everything in the process that needs to talk to the mount.
 *
 * WHY THIS EXISTS
 * ---------------
 * The first cut of the web UI reached the mount by running `polaris-mount`
 * per request. That is fine for a plate-solve dashboard that polls every few
 * seconds and fatal for a control surface:
 *
 *   * Every invocation opens a NEW TCP connection, and the device treats each
 *     one as an app connecting (SP_EVENT_APP_CONNECT). The Benro app flashes
 *     its celestial-position banner every time. Observed on hardware, which is
 *     why mount_pose() in polaris-httpd.c caches for five seconds.
 *   * fork + exec + connect + handshake per keypress is tens to hundreds of
 *     milliseconds. A joystick cannot be built on that.
 *   * A five-second cache means the UI lies about where the mount is.
 *
 * The phone app holds exactly one connection open for its whole session and
 * multiplexes everything over it. So do we.
 *
 * THE FREE SIDE EFFECT
 * --------------------
 * Sixty seconds after the last wifi client disconnects, the firmware powers the
 * radio down (see wifi-keepalive.sh). A REGISTERED connection — opcode 808 with
 * type:0, exactly what the app sends — counts as a client. So holding this link
 * open is also what keeps the radio up, and `wifi-keepalive.sh` becomes
 * redundant while the server is running. The cost is the same one that script
 * documents: the device counts us as an app, and registering while the mount is
 * UNALIGNED is what makes the Benro app demand a compass calibration. Hence
 * plink_open() is a deliberate call, not something that happens at startup.
 *
 * THREADING: none. This is driven from the caller's select() loop —
 * plink_fd() joins the read set, plink_pump() is called when it is readable
 * (and periodically regardless, so reconnect backoff and keepalives run).
 * Nothing here blocks except plink_request(), which is bounded and explicit.
 */
#ifndef POLARIS_LINK_H
#define POLARIS_LINK_H

#include <stddef.h>
#include <time.h>

/* The wire is "1&<cmd>&<type>&<k>:<v>;...#" out, "<cmd>@<k>:<v>;...#" back.
 * `type` is the app's own message class: the device log renders our frames as
 *     rcv msg from App[5]: type:2; code:808; val:type:0;
 * Registration is type 2; ordinary control is type 3. Anything else is
 * untested — pass it through rather than guessing a default. */
#define PLINK_TYPE_REGISTER 2
#define PLINK_TYPE_CONTROL  3

/* Opcodes this file needs by name. The full catalogue lives in
 * docs/APP-PROTOCOL.md; the link layer is deliberately ignorant of meaning and
 * only knows the two it must send itself. */
#define PLINK_CMD_REGISTER  808

/* How many distinct opcodes we remember the latest payload for. The device
 * pushes a modest set; 128 is far more than has ever been observed and costs
 * 16 KB. Overflow drops the OLDEST slot and is counted, never silent. */
#define PLINK_SLOTS         128
#define PLINK_ARGS_MAX      512

typedef enum {
    PLINK_DOWN = 0,      /* no socket; backoff timer running          */
    PLINK_CONNECTING,    /* connect() in flight (non-blocking)        */
    PLINK_UP             /* connected AND registered                  */
} plink_status_t;

typedef struct {
    int    cmd;                      /* opcode, e.g. 518               */
    char   args[PLINK_ARGS_MAX];     /* raw "k:v;k:v;" payload         */
    double when_ms;                  /* monotonic ms it arrived        */
    unsigned long count;             /* how many have been seen        */
} plink_slot_t;

/* Open (or re-open) the link. Idempotent: calling it while UP is a no-op.
 * Returns 0 if the attempt started, -1 on a configuration error. Connection
 * happens asynchronously — watch plink_status(). */
int  plink_open(const char *host, int port);

/* Close the link and stop reconnecting until plink_open() is called again.
 * This RELEASES the radio-keepalive side effect; say so in any UI that offers
 * it, because the wifi will drop 60 s later if nothing else is connected. */
void plink_close(void);

plink_status_t plink_status(void);
int  plink_fd(void);            /* -1 when there is no socket to select on */

/* Drive the state machine: finish a pending connect, read and parse whatever
 * arrived, run the reconnect backoff, send the periodic re-registration.
 * Safe (and required) to call even when the fd is not readable. */
void plink_pump(void);

/* Send one frame. Returns 0 if it was written or queued, -1 if the link is
 * down. `args` may be NULL for an empty payload. This does NOT wait for a
 * reply — most control opcodes answer asynchronously if at all. */
int  plink_send(int cmd, int type, const char *args);

/* Send and wait for the next reply carrying the same opcode, up to
 * timeout_ms. Returns 0 on a reply (copied into out/outcap), -1 on timeout or
 * a dead link. THIS BLOCKS THE CALLER'S EVENT LOOP — keep timeouts short and
 * prefer plink_send() plus a poll of the cache. Used for the handful of
 * requests whose answer the UI needs in the same HTTP response. */
int  plink_request(int cmd, int type, const char *args,
                   int timeout_ms, char *out, size_t outcap);

/* The latest payload seen for an opcode, or NULL if none has arrived.
 * The pointer is owned by the link and stays valid until the next pump. */
const plink_slot_t *plink_get(int cmd);

/* Iterate the cache by slot index, 0..PLINK_SLOTS-1. Returns NULL for an
 * empty or never-populated slot, so a caller can walk the whole range without
 * knowing how many opcodes have been seen. Used by the /api/link dump, which
 * exists so the UI can decode newly understood opcodes with no C change. */
const plink_slot_t *plink_get_index(int index);

/* Pull one key out of a "k:v;k:v;" payload. Returns 0 on success. Shared with
 * callers so payload parsing is written once. */
int  plink_arg(const char *args, const char *key, char *out, size_t outcap);
double plink_arg_num(const char *args, const char *key, double dflt);

/* Diagnostics for the settings/debug panel: connection counters and the last
 * error string. Never fabricates — a field it does not know reads 0/"". */
typedef struct {
    plink_status_t status;
    unsigned long  connects;        /* successful connects                */
    unsigned long  drops;           /* connections lost                   */
    unsigned long  rx_msgs;         /* frames parsed                      */
    unsigned long  tx_msgs;         /* frames written                     */
    unsigned long  parse_errors;    /* frames dropped as malformed        */
    unsigned long  slot_evictions;  /* opcode cache overflow              */
    double         up_since_ms;     /* 0 when down                        */
    double         last_rx_ms;      /* 0 when nothing has ever arrived    */
    char           last_error[128];
} plink_stats_t;

const plink_stats_t *plink_stats(void);

/* Monotonic milliseconds — exported because callers time things against the
 * same clock the slots are stamped with. */
double plink_now_ms(void);

#endif /* POLARIS_LINK_H */
