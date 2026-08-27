/* SPDX-License-Identifier: MIT
 *
 * polaris-link — see polaris-link.h for why this exists.
 *
 * Implementation notes worth keeping:
 *
 *   * NON-BLOCKING THROUGHOUT. The HTTP server this lives in handles requests
 *     inline on one thread; a blocking connect() to a mount that is asleep
 *     would hang the whole UI, and that is precisely the failure the old
 *     shell-out path produced (518 never answers on an unaligned mount, so
 *     every request waited out polaris-mount's timeout).
 *
 *   * WRITES ARE QUEUED, NOT ASSUMED. The control port is a slow embedded peer
 *     and a joystick can generate frames faster than it drains. A short write
 *     that is treated as a whole one corrupts the NEXT frame, which the device
 *     answers with silence — the worst possible symptom to debug. So partial
 *     writes park in a buffer and drain when the socket is writable.
 *
 *   * BACKOFF, NOT A RETRY STORM. The radio drops, the device sleeps, the
 *     control port goes away during a firmware operation. Reconnecting flat
 *     out would hammer a device that is already unhappy, so the delay grows
 *     1s -> 30s and resets on a successful registration.
 */
#define _GNU_SOURCE
#include "polaris-link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define RX_BUF      16384
#define TX_BUF      8192
#define BACKOFF_MIN 1000.0
#define BACKOFF_MAX 30000.0
/* The device has never been observed to drop an idle registered connection,
 * but the radio-keepalive story depends on it staying registered, so we
 * re-assert the registration periodically. Cheap insurance: one 20-byte frame
 * a minute. */
#define REREGISTER_MS 60000.0

static char           g_host[64] = "127.0.0.1";
static int            g_port = 9090;
static int            g_want_open = 0;      /* plink_open called, not closed  */
static int            g_fd = -1;
static plink_status_t g_status = PLINK_DOWN;

static char   g_rx[RX_BUF];
static size_t g_rxlen = 0;
static char   g_tx[TX_BUF];
static size_t g_txlen = 0;

static double g_next_try_ms = 0;
static double g_backoff_ms  = BACKOFF_MIN;
static double g_last_reg_ms = 0;

static plink_slot_t  g_slots[PLINK_SLOTS];
static int           g_nslots = 0;
static plink_stats_t g_stats;

/* ------------------------------------------------------------------ time */

double plink_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void note_error(const char *what) {
    snprintf(g_stats.last_error, sizeof g_stats.last_error, "%s: %s",
             what, strerror(errno));
}

/* ---------------------------------------------------------------- parsing */

int plink_arg(const char *args, const char *key, char *out, size_t outcap) {
    size_t klen;
    const char *p = args;
    if (!args || !key || !out || outcap == 0) return -1;
    klen = strlen(key);
    while (p && *p) {
        const char *colon = strchr(p, ':');
        const char *semi;
        if (!colon) return -1;
        semi = strchr(colon, ';');
        if ((size_t)(colon - p) == klen && !strncmp(p, key, klen)) {
            size_t vlen = semi ? (size_t)(semi - colon - 1) : strlen(colon + 1);
            if (vlen >= outcap) vlen = outcap - 1;
            memcpy(out, colon + 1, vlen);
            out[vlen] = 0;
            return 0;
        }
        p = semi ? semi + 1 : NULL;
    }
    return -1;
}

double plink_arg_num(const char *args, const char *key, double dflt) {
    char v[64];
    if (plink_arg(args, key, v, sizeof v) != 0 || !v[0]) return dflt;
    return atof(v);
}

/* ------------------------------------------------------------------ slots */

static plink_slot_t *slot_for(int cmd) {
    int i;
    for (i = 0; i < g_nslots; i++)
        if (g_slots[i].cmd == cmd) return &g_slots[i];
    if (g_nslots < PLINK_SLOTS) {
        plink_slot_t *s = &g_slots[g_nslots++];
        memset(s, 0, sizeof *s);
        s->cmd = cmd;
        return s;
    }
    /* Full. Evict the least recently updated rather than refusing to record —
     * a UI that silently stops seeing an opcode is harder to diagnose than a
     * counter that says the cache is too small. */
    {
        int oldest = 0;
        for (i = 1; i < g_nslots; i++)
            if (g_slots[i].when_ms < g_slots[oldest].when_ms) oldest = i;
        g_stats.slot_evictions++;
        memset(&g_slots[oldest], 0, sizeof g_slots[oldest]);
        g_slots[oldest].cmd = cmd;
        return &g_slots[oldest];
    }
}

const plink_slot_t *plink_get_index(int index) {
    if (index < 0 || index >= g_nslots) return NULL;
    if (g_slots[index].when_ms == 0.0) return NULL;
    return &g_slots[index];
}

const plink_slot_t *plink_get(int cmd) {
    int i;
    for (i = 0; i < g_nslots; i++)
        if (g_slots[i].cmd == cmd && g_slots[i].when_ms != 0.0)
            return &g_slots[i];
    return NULL;
}

/* ------------------------------------------------------------------ write */

/* Queue bytes. Never writes directly from here: ordering matters and a frame
 * written ahead of one already parked in the buffer would arrive interleaved. */
static int tx_queue(const char *s, size_t n) {
    if (g_txlen + n > sizeof g_tx) {
        /* Dropping a frame is bad, but growing without bound while the peer is
         * wedged is worse, and the joystick will send another in 100 ms. */
        snprintf(g_stats.last_error, sizeof g_stats.last_error,
                 "tx buffer full (%zu bytes), frame dropped", g_txlen);
        return -1;
    }
    memcpy(g_tx + g_txlen, s, n);
    g_txlen += n;
    return 0;
}

static void tx_drain(void) {
    while (g_txlen > 0 && g_fd >= 0) {
        ssize_t n = write(g_fd, g_tx, g_txlen);
        if (n > 0) {
            memmove(g_tx, g_tx + n, g_txlen - (size_t)n);
            g_txlen -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        note_error("write");
        plink_close();
        g_want_open = 1;               /* closed by fault, not by request */
        return;
    }
}

int plink_send(int cmd, int type, const char *args) {
    char frame[PLINK_ARGS_MAX + 64];
    int n;
    if (g_status != PLINK_UP) return -1;
    n = snprintf(frame, sizeof frame, "1&%d&%d&%s#", cmd, type, args ? args : "");
    if (n <= 0 || (size_t)n >= sizeof frame) return -1;
    if (tx_queue(frame, (size_t)n) != 0) return -1;
    g_stats.tx_msgs++;
    tx_drain();
    return 0;
}

/* ------------------------------------------------------------------- read */

/* Pull every complete "<cmd>@<args>#" out of the buffer. Mirrors
 * polaris-mount.c's conn_recv so both agree on framing, including its junk
 * guard: a buffer that fills without yielding a frame is garbage, and keeping
 * it would wedge the parser forever. */
static void rx_parse(void) {
    for (;;) {
        char *at = NULL, *hash = NULL, *p;
        for (p = g_rx; p < g_rx + g_rxlen; p++) {
            if (*p != '@' || p - g_rx < 3) continue;
            hash = memchr(p, '#', g_rxlen - (size_t)(p - g_rx));
            if (!hash) return;                 /* incomplete; wait for more */
            at = p;
            break;
        }
        if (!at) {
            if (g_rxlen == sizeof g_rx) { g_rxlen = 0; g_stats.parse_errors++; }
            return;
        }
        {
            char cmdbuf[8];
            int  cmd;
            size_t alen = (size_t)(hash - at - 1);
            plink_slot_t *s;
            memcpy(cmdbuf, at - 3, 3);
            cmdbuf[3] = 0;
            cmd = atoi(cmdbuf);
            if (alen >= PLINK_ARGS_MAX) alen = PLINK_ARGS_MAX - 1;
            s = slot_for(cmd);
            memcpy(s->args, at + 1, alen);
            s->args[alen] = 0;
            s->when_ms = plink_now_ms();
            s->count++;
            g_stats.rx_msgs++;
            g_stats.last_rx_ms = s->when_ms;
        }
        {
            size_t used = (size_t)((hash + 1) - g_rx);
            memmove(g_rx, hash + 1, g_rxlen - used);
            g_rxlen -= used;
        }
    }
}

static int rx_once(void) {
    ssize_t n;
    if (g_fd < 0) return -1;
    if (g_rxlen == sizeof g_rx) { g_rxlen = 0; g_stats.parse_errors++; }
    n = read(g_fd, g_rx + g_rxlen, sizeof g_rx - g_rxlen);
    if (n > 0) { g_rxlen += (size_t)n; rx_parse(); return 0; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n == 0) snprintf(g_stats.last_error, sizeof g_stats.last_error,
                         "control port closed the connection");
    else note_error("read");
    return -1;
}

/* ------------------------------------------------------------ connection */

static void drop(const char *why) {
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    if (g_status != PLINK_DOWN) g_stats.drops++;
    g_status = PLINK_DOWN;
    g_stats.status = g_status;
    g_stats.up_since_ms = 0;
    g_rxlen = g_txlen = 0;
    if (why && !g_stats.last_error[0])
        snprintf(g_stats.last_error, sizeof g_stats.last_error, "%s", why);
    g_next_try_ms = plink_now_ms() + g_backoff_ms;
    g_backoff_ms = g_backoff_ms * 2 > BACKOFF_MAX ? BACKOFF_MAX : g_backoff_ms * 2;
}

static void try_connect(void) {
    struct sockaddr_in sa;
    int one = 1, fl;

    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_fd < 0) { note_error("socket"); drop(NULL); return; }
    fl = fcntl(g_fd, F_GETFL, 0);
    fcntl(g_fd, F_SETFL, fl | O_NONBLOCK);
    /* CLOSE-ON-EXEC: the server shells out for solves and wifi scripts, and a
     * child inheriting this socket would keep the mount believing an app is
     * connected long after we let go. */
    fcntl(g_fd, F_SETFD, FD_CLOEXEC);
    setsockopt(g_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_port);
    if (inet_pton(AF_INET, g_host, &sa.sin_addr) != 1) {
        struct hostent *he = gethostbyname(g_host);
        if (!he) { drop("cannot resolve control host"); return; }
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    }
    if (connect(g_fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        g_status = PLINK_CONNECTING;      /* register on the next pump */
    } else if (errno == EINPROGRESS) {
        g_status = PLINK_CONNECTING;
    } else {
        note_error("connect");
        drop(NULL);
        return;
    }
    g_stats.status = g_status;
}

/* Registration is what makes the device count us as a client — both for the
 * radio-keepalive effect and for receiving pushed telemetry at all. */
static void register_link(void) {
    char frame[64];
    int n = snprintf(frame, sizeof frame, "1&%d&%d&type:0;#",
                     PLINK_CMD_REGISTER, PLINK_TYPE_REGISTER);
    if (n > 0 && tx_queue(frame, (size_t)n) == 0) g_stats.tx_msgs++;
    g_last_reg_ms = plink_now_ms();
    tx_drain();
}

int plink_open(const char *host, int port) {
    if (host && *host) snprintf(g_host, sizeof g_host, "%s", host);
    if (port > 0) g_port = port;
    g_want_open = 1;
    if (g_status != PLINK_DOWN) return 0;
    g_backoff_ms  = BACKOFF_MIN;
    g_next_try_ms = 0;                    /* try immediately */
    return 0;
}

void plink_close(void) {
    g_want_open = 0;
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_status = PLINK_DOWN;
    g_stats.status = g_status;
    g_stats.up_since_ms = 0;
    g_rxlen = g_txlen = 0;
}

plink_status_t plink_status(void) { return g_status; }
int plink_fd(void) { return g_fd; }
const plink_stats_t *plink_stats(void) { return &g_stats; }

void plink_pump(void) {
    double now = plink_now_ms();

    if (!g_want_open) return;

    if (g_status == PLINK_DOWN) {
        if (now >= g_next_try_ms) try_connect();
        return;
    }

    if (g_status == PLINK_CONNECTING) {
        /* Non-blocking connect finishes as writability; SO_ERROR carries the
         * verdict. Checking writability alone reports success for a refused
         * connection, which showed up as a link that was "up" and answered
         * nothing. */
        fd_set wf;
        struct timeval tv = {0, 0};
        int err = 0;
        socklen_t elen = sizeof err;
        FD_ZERO(&wf);
        FD_SET(g_fd, &wf);
        if (select(g_fd + 1, NULL, &wf, NULL, &tv) <= 0) return;   /* not yet */
        if (getsockopt(g_fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            errno = err ? err : errno;
            note_error("connect");
            drop(NULL);
            return;
        }
        g_status = PLINK_UP;
        g_stats.status = g_status;
        g_stats.connects++;
        g_stats.up_since_ms = now;
        g_stats.last_error[0] = 0;
        g_backoff_ms = BACKOFF_MIN;
        register_link();
        return;
    }

    /* UP */
    if (rx_once() != 0) { drop(NULL); g_want_open = 1; return; }
    tx_drain();
    if (now - g_last_reg_ms > REREGISTER_MS) register_link();
}

int plink_request(int cmd, int type, const char *args,
                  int timeout_ms, char *out, size_t outcap) {
    double deadline;
    const plink_slot_t *s;
    unsigned long before = 0;

    if (out && outcap) out[0] = 0;
    if (g_status != PLINK_UP) return -1;

    /* Correlate by "a NEW message carrying this opcode", not by "any message
     * carrying it": the device pushes some of these unsolicited, and matching
     * a stale one returns the previous answer to a fresh question. */
    s = plink_get(cmd);
    if (s) before = s->count;

    if (plink_send(cmd, type, args) != 0) return -1;

    deadline = plink_now_ms() + (timeout_ms > 0 ? timeout_ms : 800);
    for (;;) {
        fd_set rf;
        struct timeval tv;
        double left = deadline - plink_now_ms();
        if (left <= 0) break;
        if (g_fd < 0) break;
        FD_ZERO(&rf);
        FD_SET(g_fd, &rf);
        tv.tv_sec  = (long)(left / 1000.0);
        tv.tv_usec = (long)((left - tv.tv_sec * 1000.0) * 1000.0);
        if (select(g_fd + 1, &rf, NULL, NULL, &tv) > 0) {
            if (rx_once() != 0) { drop(NULL); g_want_open = 1; return -1; }
            s = plink_get(cmd);
            if (s && s->count > before) {
                if (out && outcap) snprintf(out, outcap, "%s", s->args);
                return 0;
            }
        }
    }
    return -1;
}
