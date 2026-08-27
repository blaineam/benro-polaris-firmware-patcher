/* SPDX-License-Identifier: MIT
 *
 * test_jog — prove the dead man actually stops the mount.
 *
 *   cc -O2 -std=gnu11 -Wall -Wextra polaris-link.c polaris-jog.c test_jog.c -o test_jog -lm
 *
 * This asserts on THE WIRE, not on internal state: a fake head records every
 * frame it receives, so "the axis was stopped" means a stop frame was actually
 * transmitted. Checking a struct field would pass while the head kept turning.
 */
#define _GNU_SOURCE
#include "polaris-jog.h"
#include "polaris-link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

static int fails = 0;
static void ok(int cond, const char *what) {
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

/* The fake head writes every frame it receives to a pipe the parent reads, so
 * the parent can assert on the exact command stream. */
static int  g_wire = -1;
static char g_seen[65536];
static size_t g_seenlen = 0;

static void wire_drain(void) {
    for (;;) {
        char buf[4096];
        ssize_t n = read(g_wire, buf, sizeof buf);
        if (n <= 0) return;
        if (g_seenlen + (size_t)n < sizeof g_seen - 1) {
            memcpy(g_seen + g_seenlen, buf, (size_t)n);
            g_seenlen += (size_t)n;
            g_seen[g_seenlen] = 0;
        }
    }
}
static void wire_reset(void) { wire_drain(); g_seenlen = 0; g_seen[0] = 0; }
static int  wire_saw(const char *frag) { wire_drain(); return strstr(g_seen, frag) != NULL; }
static int  wire_count(const char *frag) {
    const char *p = g_seen;
    int c = 0;
    wire_drain();
    p = g_seen;
    while ((p = strstr(p, frag)) != NULL) { c++; p++; }
    return c;
}

static int spawn_head(pid_t *child, int *wirefd) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int pfd[2];
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int one = 1, port;
    pid_t pid;

    if (pipe(pfd) != 0) { perror("pipe"); exit(2); }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("bind"); exit(2); }
    if (listen(lfd, 4) != 0) { perror("listen"); exit(2); }
    getsockname(lfd, (struct sockaddr *)&sa, &sl);
    port = ntohs(sa.sin_port);

    pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid > 0) {
        close(pfd[1]);
        { int fl = fcntl(pfd[0], F_GETFL, 0); fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK); }
        *wirefd = pfd[0];
        *child = pid;
        close(lfd);
        return port;
    }

    /* ---- child: echo every received byte to the pipe ---- */
    close(pfd[0]);
    {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) _exit(0);
        close(lfd);
        write(c, "808@ret:0;#", 11);
        for (;;) {
            char buf[2048];
            ssize_t n = read(c, buf, sizeof buf);
            if (n <= 0) _exit(0);
            write(pfd[1], buf, (size_t)n);
            /* keep the link's silence watchdog happy */
            write(c, "518@compass:1.0;alt:-1.0;#", 26);
        }
    }
}

static void pump(int ms) {
    double end = plink_now_ms() + ms;
    while (plink_now_ms() < end) { plink_pump(); jog_tick(); usleep(2000); }
}

int main(void) {
    pid_t child = 0;
    int port;

    signal(SIGPIPE, SIG_IGN);
    port = spawn_head(&child, &g_wire);
    printf("[*] fake head on :%d\n", port);

    plink_open("127.0.0.1", port);
    { double end = plink_now_ms() + 3000;
      while (plink_now_ms() < end && plink_status() != PLINK_UP) { plink_pump(); usleep(5000); } }
    ok(plink_status() == PLINK_UP, "link up");
    pump(100); wire_reset();

    /* ---- fast jog repeats on the SERVER's clock ---- */
    jog_set(JOG_PAN, 1200, 0);
    pump(260);
    ok(wire_saw("1&513&3&speed:1200;"), "fast jog sends the pan opcode with a signed speed");
    ok(wire_count("1&513&3&speed:1200;") >= 4,
       "fast jog is re-sent at ~50 ms without the client asking");

    /* ---- THE DEAD MAN: stop renewing and the axis must stop ---- */
    wire_reset();
    pump((int)JOG_LEASE_MS + 250);
    ok(wire_saw("1&513&3&speed:0;"),
       "an unrenewed lease STOPS the axis (client disappeared)");
    ok(!jog_active(), "nothing is left active after the lease expires");
    {   /* and it must stop repeating, not keep streaming zeros forever */
        int before;
        wire_reset(); pump(300);
        before = wire_count("1&513&");
        ok(before == 0, "no frames are sent for a stopped axis");
    }

    /* ---- renewal keeps it alive ---- */
    wire_reset();
    { int i; for (i = 0; i < 6; i++) { jog_set(JOG_TILT, -900, 0); pump(120); } }
    ok(wire_saw("1&514&3&speed:-900;"), "tilt jogs negative with the sign preserved");
    ok(!wire_saw("1&514&3&speed:0;"), "a renewed lease is NOT stopped mid-hold");
    jog_stop(JOG_TILT); pump(60);
    ok(wire_saw("1&514&3&speed:0;"), "explicit stop is sent");

    /* ---- speed clamping ---- */
    wire_reset();
    jog_set(JOG_PAN, 99999, 0); pump(60);
    ok(wire_saw("1&513&3&speed:2500;"), "speed clamps to the head's maximum");
    jog_set(JOG_PAN, 3, 0); pump(60);
    ok(wire_saw("1&513&3&speed:100;"), "a magnitude below the head's floor is raised, not sent as-is");
    jog_stop_all(); pump(60);

    /* ---- latched slow jog: press once, release once ---- */
    wire_reset();
    { int i; for (i = 0; i < 5; i++) { jog_set(JOG_ROT, 0, 3); pump(120); } }
    ok(wire_count("1&534&3&key:0;state:1;level:3;") == 1,
       "a latched press is sent ONCE however many times the lease is renewed");
    ok(!wire_saw("level:0;"), "level 0 is never sent (it disengages torque)");
    wire_reset();
    pump((int)JOG_LEASE_MS + 250);
    ok(wire_saw("1&534&3&key:0;state:0;level:3;"),
       "the latched press is RELEASED when the lease expires");

    /* ---- direction change releases the old press first ---- */
    wire_reset();
    jog_set(JOG_ROT, 0, 2); pump(60);
    jog_set(JOG_ROT, 0, -2); pump(60);
    ok(wire_count("state:0;") >= 1, "changing direction releases the previous press");
    ok(wire_saw("key:1;state:1;level:2;"), "the new direction is pressed with key:1");
    jog_stop_all(); pump(60);

    /* ---- switching families does not strand a latch ---- */
    wire_reset();
    jog_set(JOG_PAN, 0, 4); pump(60);          /* latched */
    jog_set(JOG_PAN, 800, 0); pump(60);        /* switch to continuous */
    ok(wire_saw("1&532&3&key:0;state:0;level:4;"),
       "switching from latched to continuous releases the latch");
    jog_stop_all(); pump(60);

    /* ---- a dead link clears intents rather than leaving them latched ---- */
    jog_set(JOG_PAN, 1500, 0); pump(60);
    ok(jog_active(), "active before the link drops");
    plink_close();
    jog_tick();
    ok(!jog_active(), "a dropped link clears every intent");

    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
