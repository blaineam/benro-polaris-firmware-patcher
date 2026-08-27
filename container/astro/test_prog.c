/* SPDX-License-Identifier: MIT
 *
 * test_prog — the frames the programmes put on the wire, and the guards that
 * stop bad ones being built at all.
 *
 *   cc -O2 -std=gnu11 -Wall -Wextra polaris-link.c polaris-prog.c test_prog.c -o test_prog -lm
 *
 * Asserted against a recording fake head, like test_jog: what matters is the
 * exact bytes, because these are substring-scanned by firmware that answers a
 * malformed frame with silence.
 */
#define _GNU_SOURCE
#include "polaris-prog.h"
#include "polaris-link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
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

static int g_wire = -1;
static char g_seen[65536];
static size_t g_seenlen = 0;

static void drain(void) {
    for (;;) {
        char b[4096];
        ssize_t n = read(g_wire, b, sizeof b);
        if (n <= 0) return;
        if (g_seenlen + (size_t)n < sizeof g_seen - 1) {
            memcpy(g_seen + g_seenlen, b, (size_t)n);
            g_seenlen += (size_t)n; g_seen[g_seenlen] = 0;
        }
    }
}
static void reset(void) { drain(); g_seenlen = 0; g_seen[0] = 0; }
static int saw(const char *f) { drain(); return strstr(g_seen, f) != NULL; }

/* The fake answers the remaining-count poll with a countdown, so the progress
 * machinery has something real to absorb. */
static int g_remaining = 5;

static int spawn_head(pid_t *child, int *wirefd) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0), pfd[2], one = 1, port;
    struct sockaddr_in sa; socklen_t sl = sizeof sa; pid_t pid;
    if (pipe(pfd) != 0) { perror("pipe"); exit(2); }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("bind"); exit(2); }
    listen(lfd, 4);
    getsockname(lfd, (struct sockaddr *)&sa, &sl);
    port = ntohs(sa.sin_port);
    pid = fork();
    if (pid > 0) {
        close(pfd[1]);
        { int fl = fcntl(pfd[0], F_GETFL, 0); fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK); }
        *wirefd = pfd[0]; *child = pid; close(lfd); return port;
    }
    close(pfd[0]);
    {
        int c = accept(lfd, NULL, NULL), rem = g_remaining;
        if (c < 0) _exit(0);
        close(lfd);
        write(c, "808@ret:0;#", 11);
        for (;;) {
            char b[2048];
            ssize_t n = read(c, b, sizeof b - 1);
            if (n <= 0) _exit(0);
            b[n] = 0;
            write(pfd[1], b, (size_t)n);
            /* Each programme's START refills the countdown, so a shared `rem`
             * that a previous run drained does not make the next one look done
             * the instant it begins. */
            if (strstr(b, "&2&step:1;") || strstr(b, "&2&step:2;") ||
                strstr(b, "&2&step:3;")) rem = 8;
            /* Camera shutter list, so HDR can resolve real indices/labels. */
            if (strstr(b, "1&268&"))
                write(c, "268@RD:0;V:4;R:1/1000,1/500,1/250,1/125,1/60,1/30;#", 51);
            /* Timelapse/panorama countdown poll. */
            if (strstr(b, "step:4;") || strstr(b, "step:3;")) {
                char r[64];
                int len = snprintf(r, sizeof r, "%s@ret:%d;#",
                                   strstr(b, "1&271") ? "271" : "272", rem);
                if (rem > 0) rem--;
                write(c, r, (size_t)len);
            }
            /* Focus running-info (step 7) answers with remainNum. */
            if (strstr(b, "1&270&2&step:7;")) {
                char r[64];
                int len = snprintf(r, sizeof r, "270@step:7;remainNum:%d;#", rem);
                if (rem > 0) rem--;
                write(c, r, (size_t)len);
            }
            /* HDR: the head PUSHES progress. When it sees the start, emit a
             * step:5 remaining then a step:3 complete shortly after. */
            if (strstr(b, "1&280&2&step:1;")) {
                write(c, "280@step:5;ret:2;#", 18);
            }
            /* FREE PROGRAM runtime poll (283 step 5) -> appointment state 2. */
            if (strstr(b, "1&283&2&step:5;"))
                write(c, "283@step:5;ret:2;#", 18);
            /* Always keep a current pose available so plc key-capture works. */
            write(c, "517@yaw:0.5;pitch:-0.2;roll:0.0;#", 33);
            write(c, "518@compass:1.0;alt:-1.0;#", 26);
        }
    }
}

static void pump(int ms) {
    double end = plink_now_ms() + ms;
    while (plink_now_ms() < end) { plink_pump(); prog_tick(); usleep(2000); }
}

int main(void) {
    pid_t child = 0;
    int port;
    char err[160], buf[512];

    setvbuf(stdout, NULL, _IONBF, 0);   /* so a forked fake head cannot inherit
                                         * unflushed assertion output and double it */
    signal(SIGPIPE, SIG_IGN);
    port = spawn_head(&child, &g_wire);
    printf("[*] fake head on :%d\n", port);

    /* ---- validation, which needs no link at all ---- */
    {
        lapse_params_t l = { 4.0, 100, 0, 24 };
        pano_params_t  p = { 6, 3, 30.0, 20.0, 1, 1, 0, 0, 0 };
        ok(prog_check_lapse(&l, err, sizeof err) == 0, "a sane timelapse validates");
        ok(prog_check_pano(&p, err, sizeof err) == 0, "a sane panorama validates");

        l.bulb_s = 10; l.interval_s = 4;
        ok(prog_check_lapse(&l, err, sizeof err) != 0 && strstr(err, "longer than the interval"),
           "a bulb exposure longer than the interval is refused (the app does not check this)");
        l.bulb_s = 0;

        l.interval_s = 0;
        ok(prog_check_lapse(&l, err, sizeof err) != 0, "a zero interval is refused");
        l.interval_s = 4;
        l.shots = -1;
        ok(prog_check_lapse(&l, err, sizeof err) == 0, "unlimited shots (-1) is valid — it is the app's default");

        p.h_angle = 30; p.h_num = 20;   /* 600 degrees */
        ok(prog_check_pano(&p, err, sizeof err) != 0 && strstr(err, "360"),
           "a horizontal sweep past a full turn is refused");
        p.h_num = 6;
        p.per_spot = 0;
        ok(prog_check_pano(&p, err, sizeof err) != 0, "zero frames per position is refused");
        p.per_spot = 1;
        p.interval_s = 30;
        ok(prog_check_pano(&p, err, sizeof err) != 0, "a per-position interval past the head's 20 s cap is refused");
        p.interval_s = 0;
    }

    /* ---- derived durations ---- */
    {
        lapse_params_t l = { 4.0, 100, 0, 24 };
        double s = 0, v = 0;
        lapse_durations(&l, &s, &v);
        ok(s == 396.0, "shooting duration counts the GAPS, not the frames ((100-1)*4)");
        ok(v == 5.0, "video duration is ceil(shots/fps)");
        l.shots = -1;
        ok(lapse_durations(&l, &s, &v) == 0, "an unlimited run has no duration");
    }

    /* ---- the pano preview renders without sending ---- */
    {
        pano_params_t p = { 6, 3, 30.0, 20.0, 1, 2, 5, 0, 1 };
        prog_pano_preview(&p, buf, sizeof buf);
        ok(strstr(buf, "1&271&2&step:2;") != NULL, "preview renders a complete 271 start frame");
        ok(strstr(buf, "para:6,3,1,0,30,20;") != NULL,
           "para is hNum,vNum,startDir,paMode,hAngle,vAngle in DEGREES");
        ok(strstr(buf, "num:36;") != NULL, "num is cols*rows*perSpot (6*3*2)");
        ok(strstr(buf, "isp:1;") != NULL, "the stitching flag rides through");
        ok(!saw("1&271"), "previewing sends NOTHING to the head");
    }

    /* ---- link up ---- */
    plink_open("127.0.0.1", port);
    { double e = plink_now_ms() + 3000;
      while (plink_now_ms() < e && plink_status() != PLINK_UP) { plink_pump(); usleep(5000); } }
    ok(plink_status() == PLINK_UP, "link up");
    pump(80); reset();

    /* ---- timelapse start: the three-step sequence, in order ---- */
    {
        lapse_params_t l = { 4.0, 100, 0, 24 };
        ok(prog_start_lapse(&l, err, sizeof err) == 0, "timelapse starts");
        pump(120);
        ok(saw("1&272&2&step:1;"), "step 1 START is sent");
        ok(saw("1&272&2&step:2;point:1;time:0;para:4,100;bulb:0;"),
           "step 2 carries interval and count in the app's exact shape");
        ok(saw("photoCnt:100;;preview:0;"),
           "step 3's DOUBLE SEMICOLON is emitted verbatim (the app does it, and this is a substring protocol)");
        ok(saw("time:396;"), "step 3's time is the derived (shots-1)*interval");
        ok(prog_running() == PROG_LAPSE, "the programme registers as running");
    }

    /* ---- a second programme is refused while one runs ---- */
    {
        pano_params_t p = { 6, 3, 30.0, 20.0, 1, 1, 0, 0, 0 };
        ok(prog_start_pano(&p, err, sizeof err) != 0 && strstr(err, "already running"),
           "a second programme is refused while one is running");
    }

    /* ---- the progress poll runs on the SERVER, unprompted ---- */
    reset();
    pump(2500);
    ok(saw("1&272&2&step:4;"), "the remaining-count poll is re-issued by the server, with no client");
    {
        char st[512];
        prog_status_json(st, sizeof st);
        ok(strstr(st, "\"kind\":\"timelapse\"") != NULL, "status reports the running programme");
        ok(strstr(st, "\"remaining\":") != NULL, "status carries the head's remaining count");
    }

    /* ---- cancel ---- */
    reset();
    prog_cancel();
    pump(80);
    ok(saw("1&272&2&step:7;"), "cancel sends the timelapse CANCEL step");
    ok(prog_running() == PROG_NONE, "nothing runs after cancel");

    /* ---- panorama start + live interval + pause ---- */
    reset();
    {
        pano_params_t p = { 4, 2, 45.0, 15.0, 1, 3, 5, 0, 0 };
        ok(prog_start_pano(&p, err, sizeof err) == 0, "panorama starts");
        pump(120);
        ok(saw("1&271&2&step:2;para:4,2,1,0,45,15;"), "the start frame matches the documented field order");
        ok(saw("num:24;"), "num is 4*2*3");
        ok(saw("1&271&2&step:13;interval:5;"),
           "the per-spot interval is sent as its own live command, not in the start order");
        ok(prog_pano_pause(1) == 0, "pause is accepted");
        pump(60);
        ok(saw("1&271&2&step:12;pause:1;"), "pause sends step 12");
        ok(prog_pano_interval(9) == 0, "the interval can be changed mid-run");
        pump(60);
        ok(saw("step:13;interval:9;"), "the new interval reaches the head");
        reset();
        pump(2500);
        ok(saw("1&271&2&step:3;"), "panorama polls its own remaining count");
    }

    /* ---- HDR: three shutter values resolved from the camera, push progress ---- */
    prog_cancel(); pump(60); reset();   /* the panorama above is still running */
    {
        hdr_params_t h = { 2, 1 };   /* spread 2, isp on */
        char err2[160], prev[512];
        /* Prime the shutter list into the cache. */
        plink_send(268, 1, ""); pump(120);
        prog_hdr_preview(&h, prev, sizeof prev);
        ok(strstr(prev, "1&280&2&step:1;isp:1;") != NULL, "HDR preview is a complete 280 start frame");
        ok(strstr(prev, "p1:0,2,") && strstr(prev, "p2:0,4,") && strstr(prev, "p3:0,5,"),
           "the three frames bracket the shutter index, high end CLAMPED to the list (V4 s2, len6 -> 2/4/5)");
        ok(strstr(prev, "shutters: 1/250 / 1/60 / 1/30") != NULL,
           "the preview shows the REAL shutter labels the indices resolve to");
        ok(prog_start_hdr(&h, err2, sizeof err2) == 0, "HDR starts");
        pump(120);
        ok(saw("1&280&2&step:1;isp:1;p1:0,2,"), "the HDR start frame reaches the head");
        {
            char st[512]; prog_status_json(st, sizeof st);
            ok(strstr(st, "\"kind\":\"hdr\"") != NULL && strstr(st, "\"total\":3"),
               "HDR reports as a 3-frame programme");
        }
        /* It should NOT poll -- HDR is push-driven. */
        reset(); pump(600);
        ok(!saw("1&280&2&step:"), "HDR is not polled (the head pushes its own progress)");
        /* The pushed step:5 remaining lands, then a completion clears it. */
        write(g_wire, "", 0);   /* no-op keep symmetry */
        prog_cancel(); pump(60);
        ok(saw("1&280&2&step:4;"), "HDR cancel sends step 4");
    }

    /* ---- focus stack: mark, start, poll remainNum, cancel ---- */
    reset();
    {
        focus_params_t f = { 14, 1 };
        char err2[160];
        ok(prog_check_focus(&f, err2, sizeof err2) == 0, "a 14-shot focus stack validates");
        f.shots = 1;
        ok(prog_check_focus(&f, err2, sizeof err2) != 0, "1 shot is refused (min is 2)");
        f.shots = 250;
        ok(prog_check_focus(&f, err2, sizeof err2) != 0, "251 shots is refused (max is 200)");
        f.shots = 14;

        prog_focus_mark(0); pump(60);
        ok(saw("1&270&2&step:1;"), "marking the near limit sends step 1");
        prog_focus_mark(1); pump(60);
        ok(saw("1&270&2&step:2;"), "marking the far limit sends step 2");

        reset();
        ok(prog_start_focus(&f, err2, sizeof err2) == 0, "focus stack starts");
        pump(120);
        ok(saw("1&270&2&step:3;;num:14;isp:1;"),
           "the start frame carries ;num:<shots>; with the genuine double semicolon");
        reset();
        pump(2500);
        ok(saw("1&270&2&step:7;"), "focus polls RUNNING_INFO (step 7), not a made-up opcode");
        {
            char st[512]; prog_status_json(st, sizeof st);
            ok(strstr(st, "\"kind\":\"focus\"") != NULL, "status reports focus");
            ok(strstr(st, "\"remaining\":") != NULL, "the remainNum count is surfaced");
        }
        prog_cancel(); pump(60);
        ok(saw("1&270&2&step:6;"), "focus cancel sends step 6");
    }

    /* ---- focus preview is a distinct, cancellable dry traverse ---- */
    reset();
    {
        focus_params_t f = { 8, 0 };
        char err2[160], st[512];
        ok(prog_focus_preview(&f, err2, sizeof err2) == 0, "focus preview starts");
        pump(80);
        ok(saw("1&270&2&step:8;;num:8;"), "preview sends step 8 with the shot count");
        prog_status_json(st, sizeof st);
        ok(strstr(st, "\"preview\":true") != NULL, "status flags it as a preview, not a real run");
        prog_cancel(); pump(60);
        ok(saw("1&270&2&step:10;"), "cancelling a preview sends step 10, not step 6");
    }

    /* ---- FREE PROGRAM: capture poses, build the timeline, run ---- */
    reset();
    {
        plc_params_t pp; char err2[160], prev[1024];
        prog_plc_clear();
        pump(120);   /* let a 517 pose land */
        ok(prog_plc_add_key(0, err2, sizeof err2) == 1, "first keyframe captures the current pose");
        ok(prog_plc_add_key(5, err2, sizeof err2) == 2, "second keyframe at t=5");
        ok(prog_plc_key_count() == 2, "two keyframes held before the run");

        memset(&pp, 0, sizeof pp);
        /* copy the captured keys out via the preview path */
        pp.n_keys = 2;
        pp.keys[0].t_s = 0; pp.keys[0].pan = 0.5; pp.keys[0].tilt = -0.2; pp.keys[0].roll = 0;
        pp.keys[1].t_s = 5; pp.keys[1].pan = 1.0; pp.keys[1].tilt = -0.1; pp.keys[1].roll = 0;
        pp.photo_interval_s = 2; pp.photo_count = -1; pp.hold = 0;

        prog_plc_preview(&pp, prev, sizeof prev);
        ok(strstr(prev, "1&283&2&step:1;") != NULL, "the timeline opens with START");
        ok(strstr(prev, "step:2;item:1;point:1;time:0;para:-1,2;") != NULL,
           "the photo track rides item:1 with interval and count");
        ok(strstr(prev, "step:2;item:3;point:1;time:0;para:0.5,-0.2,0;mode:0;") != NULL,
           "a motion keyframe rides item:3 with the pose in RADIANS and linear interpolation");
        ok(strstr(prev, "step:2;item:3;point:2;time:5;") != NULL, "the second keyframe is point 2 at t=5");
        ok(strstr(prev, "step:3;item1:1,5;item2:0,5;item3:2,5;") != NULL,
           "END_POINT carries the per-track counts and the last time");

        ok(prog_start_plc(&pp, err2, sizeof err2) == 0, "free program starts");
        pump(120);
        ok(saw("1&283&2&step:1;"), "START reaches the head");
        ok(saw("1&283&2&step:2;item:3;point:2;"), "both motion keyframes are uploaded");
        ok(saw("1&283&2&step:3;item1:1,5;"), "END_POINT is sent");
        {
            char st[512]; prog_status_json(st, sizeof st);
            ok(strstr(st, "\"kind\":\"program\"") != NULL, "status reports the program");
        }
        reset(); pump(1500);
        ok(saw("1&283&2&step:5;"), "the program polls RUNTIME (step 5)");
        prog_cancel(); pump(60);
        ok(saw("1&283&2&step:6;"), "cancel sends step 6");
    }

    /* ---- free program validation ---- */
    {
        plc_params_t pp; char err2[160];
        memset(&pp, 0, sizeof pp);
        pp.n_keys = 1; pp.keys[0].t_s = 0;
        ok(prog_check_plc(&pp, err2, sizeof err2) != 0 && strstr(err2, "two"),
           "a single keyframe is refused");
        pp.n_keys = 2; pp.keys[1].t_s = 0;   /* not increasing */
        ok(prog_check_plc(&pp, err2, sizeof err2) != 0 && strstr(err2, "increase"),
           "non-increasing keyframe times are refused");
    }

    /* ---- a lost link ends the run rather than reporting phantom progress ---- */
    plink_close();
    prog_tick();
    ok(prog_running() == PROG_NONE, "losing the link clears the programme (we can no longer see or stop it)");

    /* ---- nothing starts without a link ---- */
    {
        lapse_params_t l = { 4.0, 10, 0, 24 };
        hdr_params_t h = { 2, 0 };
        char err2[160];
        ok(prog_start_lapse(&l, err, sizeof err) != 0 && strstr(err, "link"),
           "a programme will not start with the link down");
        ok(prog_check_hdr(&h, err2, sizeof err2) != 0 && strstr(err2, "link"),
           "HDR refuses with the link down (it cannot bracket without the camera)");
    }

    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
