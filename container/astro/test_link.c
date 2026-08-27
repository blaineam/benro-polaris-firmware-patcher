/* SPDX-License-Identifier: MIT
 *
 * test_link — exercise polaris-link against a peer that speaks the mount's
 * protocol. Built and run on the HOST (no device, no cross-compiler): the link
 * layer is pure sockets and parsing, and every bug it can have shows up here.
 *
 *   cc -O2 -std=gnu11 -Wall -Wextra polaris-link.c test_link.c -o test_link
 *   ./test_link            # self-contained: forks its own fake mount
 *   ./test_link 9090       # or point it at polaris-sim.py on a port
 *
 * The built-in fake is deliberately nastier than the simulator: it pushes
 * telemetry in TORN writes (a frame split across two packets, two frames in
 * one packet) because that is what a real socket does and it is exactly what
 * a naive parser gets wrong.
 */
#define _GNU_SOURCE
#include "polaris-link.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int fails = 0;

static void ok(int cond, const char *what) {
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

/* ------------------------------------------------------- the fake mount */

/* Returns the listening port; the child serves exactly one connection and
 * then keeps it open, pushing telemetry, until the parent goes away. */
static int spawn_fake_mount(pid_t *child) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int one = 1, port;
    pid_t pid;

    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;                              /* let the kernel pick */
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("bind"); exit(2); }
    if (listen(lfd, 4) != 0) { perror("listen"); exit(2); }
    getsockname(lfd, (struct sockaddr *)&sa, &sl);
    port = ntohs(sa.sin_port);

    pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid > 0) { *child = pid; close(lfd); return port; }

    /* ---- child ---- */
    {
        int c = accept(lfd, NULL, NULL);
        char rx[4096];
        size_t rxlen = 0;
        int saw_register = 0, i;
        if (c < 0) _exit(0);
        close(lfd);

        /* Wait for the registration frame, then answer it the way the device
         * does (the log shows SP_SendMsgToApp: code[808], val[ret:0;]). */
        while (!saw_register) {
            ssize_t n = read(c, rx + rxlen, sizeof rx - rxlen - 1);
            if (n <= 0) _exit(0);
            rxlen += (size_t)n;
            rx[rxlen] = 0;
            if (strstr(rx, "1&808&2&type:0;#")) saw_register = 1;
        }
        write(c, "808@ret:0;#", 11);

        /* A frame split across two writes. A parser that assumes one read ==
         * one frame loses this one. */
        write(c, "518@w:1.0;x:0.0;y:0.0;z:0.0;compass:162.07", 42);
        usleep(120000);
        write(c, "4631;alt:-28.265282;#", 21);

        /* Two frames in a single write, plus a leading fragment of junk that
         * the junk guard must not choke on. */
        write(c, "284@mode:8;track:1;#517@yaw:2.83;pitch:-0.49;roll:0.0;#", 55);

        /* Answer a request-style opcode so plink_request() has something to
         * correlate. 284 is asked for and also pushed, which is the case that
         * catches "matched a stale message". */
        for (i = 0; i < 200; i++) {
            char buf[512];
            ssize_t n;
            usleep(50000);
            n = read(c, buf, sizeof buf - 1);
            if (n > 0) {
                buf[n] = 0;
                if (strstr(buf, "1&284&3&")) write(c, "284@mode:8;track:0;#", 20);
                if (strstr(buf, "1&519&3&")) write(c, "519@ret:0;#", 11);
            } else if (n == 0) break;
            /* keep pushing pose so the cache has fresh data */
            if ((i % 10) == 0) write(c, "518@compass:99.5;alt:-10.0;#", 28);
        }
        _exit(0);
    }
}

/* Pump the link until `pred` holds or the budget runs out. Returns 1 if the
 * predicate became true. */
static int pump_until(int (*pred)(void), int budget_ms) {
    double deadline = plink_now_ms() + budget_ms;
    while (plink_now_ms() < deadline) {
        plink_pump();
        if (pred()) return 1;
        usleep(10000);
    }
    plink_pump();
    return pred();
}

static int is_up(void)        { return plink_status() == PLINK_UP; }
static int have_pose(void)    { return plink_get(518) != NULL; }
static int have_mode(void)    { return plink_get(284) != NULL; }
static int have_raw(void)     { return plink_get(517) != NULL; }

int main(int argc, char **argv) {
    pid_t child = 0;
    int port, own_peer;
    char out[256];

    signal(SIGPIPE, SIG_IGN);

    own_peer = (argc <= 1);
    if (!own_peer) { port = atoi(argv[1]); printf("[*] using an external peer on :%d\n", port); }
    else           { port = spawn_fake_mount(&child); printf("[*] fake mount on :%d\n", port); }

    /* ---- arg parsing, which needs no socket at all ---- */
    {
        char v[64];
        const char *args = "w:1.0;x:0.0;compass:162.074631;alt:-28.265282;";
        ok(plink_arg(args, "compass", v, sizeof v) == 0 && !strcmp(v, "162.074631"),
           "plink_arg reads a middle key");
        ok(plink_arg(args, "alt", v, sizeof v) == 0 && !strcmp(v, "-28.265282"),
           "plink_arg reads the last key");
        ok(plink_arg(args, "w", v, sizeof v) == 0 && !strcmp(v, "1.0"),
           "plink_arg reads the first key");
        ok(plink_arg(args, "compas", v, sizeof v) != 0,
           "plink_arg does not match a key PREFIX");
        ok(plink_arg(args, "x", v, sizeof v) == 0 && !strcmp(v, "0.0"),
           "plink_arg does not match a key SUFFIX");   /* 'x' must not hit 'w' or 'compass' */
        ok(plink_arg_num(args, "alt", 999) == -28.265282, "plink_arg_num parses a negative");
        ok(plink_arg_num(args, "nope", 42) == 42, "plink_arg_num falls back to the default");
    }

    /* ---- connect + register ---- */
    ok(plink_status() == PLINK_DOWN, "starts down");
    ok(plink_send(518, PLINK_TYPE_CONTROL, NULL) == -1, "send refuses while down");
    plink_open("127.0.0.1", port);
    ok(pump_until(is_up, 3000), "connects and registers");

    /* ---- the torn frame ---- */
    ok(pump_until(have_pose, 3000), "parses a frame split across two reads");
    if (plink_get(518)) {
        /* Against our own fake the value is the exact constant it sent, and
         * checking it to 1e-6 is what proves the torn frame was rejoined
         * rather than truncated at the split. An external peer (polaris-sim,
         * or real hardware) reports its OWN live azimuth, so there the only
         * honest assertion is that a well-formed azimuth arrived at all —
         * asserting the constant there fails for a reason that has nothing to
         * do with the code under test. */
        const plink_slot_t *s = plink_get(518);
        double az = plink_arg_num(s->args, "compass", -999);
        if (own_peer)
            ok(fabs(az - 162.074631) < 1e-6, "the split frame's value survived intact");
        else
            ok(az > -360.0 && az < 360.0 && strstr(s->args, "alt:") != NULL,
               "peer's pose parsed into a plausible azimuth + alt");
    } else fails++;

    /* ---- two frames in one read ---- */
    ok(pump_until(have_mode, 3000) && pump_until(have_raw, 500),
       "parses two frames delivered in one read");
    if (plink_get(284))
        ok(plink_arg_num(plink_get(284)->args, "mode", -1) == 8, "mode parsed from the batched frame");

    /* ---- request/response correlation ---- */
    {
        const plink_slot_t *s = plink_get(284);
        unsigned long before = s ? s->count : 0;
        int r = plink_request(284, PLINK_TYPE_CONTROL, NULL, 1500, out, sizeof out);
        ok(r == 0, "plink_request gets an answer");
        ok(plink_get(284)->count > before, "the answer is a NEW message, not the cached one");
        ok(strstr(out, "track:0") != NULL, "the reply payload is returned to the caller");
    }

    /* ---- stats are real ---- */
    {
        const plink_stats_t *st = plink_stats();
        ok(st->connects == 1, "one connect counted");
        ok(st->rx_msgs >= 4, "received frames counted");
        ok(st->tx_msgs >= 2, "sent frames counted (registration + request)");
        ok(st->parse_errors == 0, "no parse errors on well-formed input");
    }

    /* ---- close releases everything ---- */
    plink_close();
    ok(plink_status() == PLINK_DOWN, "close puts it back down");
    ok(plink_fd() == -1, "close releases the fd");
    ok(plink_send(518, PLINK_TYPE_CONTROL, NULL) == -1, "send refuses after close");

    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
