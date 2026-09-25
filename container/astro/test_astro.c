/* SPDX-License-Identifier: MIT
 *
 * test_astro — the astro session's frames: enter/leave, the AHRS heartbeat,
 * compass alignment, and tracking. Asserted on the wire against a recording
 * fake head, like the others.
 *
 *   cc -O2 -std=gnu11 -Wall -Wextra polaris-link.c polaris-astro.c test_astro.c -o test_astro -lm
 */
#define _GNU_SOURCE
#include "polaris-astro.h"
#include "polaris-link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int fails = 0;
static void ok(int c, const char *what) {
    printf("%s  %s\n", c ? "ok  " : "FAIL", what);
    if (!c) fails++;
}

static int  g_wire = -1;
static char g_seen[65536];
static size_t g_seenlen = 0;
static void drain(void) {
    for (;;) { char b[4096]; ssize_t n = read(g_wire, b, sizeof b);
        if (n <= 0) return;
        if (g_seenlen + (size_t)n < sizeof g_seen - 1) { memcpy(g_seen+g_seenlen,b,(size_t)n); g_seenlen+=(size_t)n; g_seen[g_seenlen]=0; } }
}
static void reset(void) { drain(); g_seenlen = 0; g_seen[0] = 0; }
static int  saw(const char *f) { drain(); return strstr(g_seen, f) != NULL; }
static int  cnt(const char *f) { const char *p; int c=0; drain(); p=g_seen; while((p=strstr(p,f))){c++;p++;} return c; }

static int spawn_head(pid_t *child, int *wirefd) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0), pfd[2], one = 1, port;
    struct sockaddr_in sa; socklen_t sl = sizeof sa; pid_t pid;
    if (pipe(pfd) != 0) { perror("pipe"); exit(2); }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("bind"); exit(2); }
    listen(lfd, 4); getsockname(lfd, (struct sockaddr *)&sa, &sl); port = ntohs(sa.sin_port);
    pid = fork();
    if (pid > 0) { close(pfd[1]); { int fl=fcntl(pfd[0],F_GETFL,0); fcntl(pfd[0],F_SETFL,fl|O_NONBLOCK); } *wirefd=pfd[0]; *child=pid; close(lfd); return port; }
    close(pfd[0]);
    { int c = accept(lfd, NULL, NULL); if (c < 0) _exit(0); close(lfd);
      write(c, "808@ret:0;#", 11);
      for (;;) { char b[2048]; ssize_t n = read(c, b, sizeof b); if (n<=0) _exit(0);
        write(pfd[1], b, (size_t)n);
        write(c, "518@compass:1.0;alt:-1.0;#", 26); } }
    return 0;
}

static void pump(int ms) {
    double end = plink_now_ms() + ms;
    while (plink_now_ms() < end) { plink_pump(); astro_tick(); usleep(2000); }
}

int main(void) {
    pid_t child = 0; int port; char err[160];
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    port = spawn_head(&child, &g_wire);
    printf("[*] fake head on :%d\n", port);

    ok(astro_active() == 0, "starts inactive");
    ok(astro_enter(40.0, -111.0, err, sizeof err) != 0, "cannot enter with the link down");

    plink_open("127.0.0.1", port);
    { double e = plink_now_ms() + 3000; while (plink_now_ms() < e && plink_status() != PLINK_UP) { plink_pump(); usleep(5000); } }
    pump(80); reset();

    /* ---- enter: mode 8 + AHRS on ---- */
    ok(astro_enter(40.0, -111.0, err, sizeof err) == 0, "enters the astro session");
    pump(80);
    ok(saw("1&285&2&mode:8;"), "entering sends SP_SET_MODE_STATE(8)");
    ok(saw("1&520&2&state:1;"), "and enables the AHRS attitude stream");
    ok(astro_active(), "the session is active");

    /* ---- the AHRS heartbeat re-arms on its own ---- */
    reset();
    pump(9000);   /* > 2 heartbeat intervals */
    ok(cnt("1&520&2&state:1;") >= 2, "AHRS is re-armed on a timer without the client asking");

    /* ---- compass alignment: the browser stand-in for the phone magnetometer ---- */
    reset();
    ok(astro_align(123.5, err, sizeof err) == 0, "alignment accepts a heading");
    pump(60);
    ok(saw("1&527&3&compass:123.5000;lat:40.000000;lng:-111.000000;"),
       "527 carries the heading AND the server's own lat/lon, exactly as the app serialises it");
    ok(astro_align(400, err, sizeof err) != 0 && strstr(err, "0..360"),
       "a heading outside 0..360 is refused");

    /* ---- tracking, with the half-rate inversion ---- */
    reset();
    ok(astro_track(1, ASTRO_RATE_SIDEREAL, 0, err, sizeof err) == 0, "tracking starts");
    pump(60);
    ok(saw("1&536&3&halfSpeed:0;"), "full rate sends halfSpeed:0");
    ok(saw("1&531&3&state:1;speed:0;"), "531 starts sidereal tracking");
    reset();
    ok(astro_track(1, ASTRO_RATE_LUNAR, 1, err, sizeof err) == 0, "can switch to lunar + half rate");
    pump(60);
    ok(saw("1&536&3&halfSpeed:1;"), "HALF rate is halfSpeed:1 on the wire (the app double-inverts it)");
    ok(saw("1&531&3&state:1;speed:2;"), "lunar rate is speed:2");
    reset();
    ok(astro_track(0, 0, 0, err, sizeof err) == 0, "tracking stops");
    pump(60);
    ok(saw("1&531&3&state:0;"), "stop sends state:0");

    /* ---- leave: stops tracking, disables AHRS, restores photo mode ---- */
    reset();
    astro_track(1, ASTRO_RATE_SIDEREAL, 0, err, sizeof err);   /* leave WHILE tracking */
    pump(40); reset();
    astro_leave();
    pump(40);
    ok(saw("1&531&3&state:0;"), "leaving while tracking stops tracking first");
    ok(saw("1&520&2&state:0;"), "and disables AHRS (leaving it on drains the battery)");
    ok(saw("1&285&2&mode:1;"), "and returns the head to plain photo mode");
    ok(!astro_active(), "the session is closed");

    /* ---- a lost link folds the session ---- */
    astro_enter(40.0, -111.0, err, sizeof err);
    pump(40);
    ok(astro_active(), "active again");
    plink_close();
    astro_tick();
    ok(!astro_active(), "a dropped link closes the astro session (AHRS can no longer be kept alive)");

    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
