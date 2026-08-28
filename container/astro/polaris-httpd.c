/* ===========================================================================
 * polaris-httpd -- tiny HTTP server for the Benro Polaris plate solver.
 *
 * Serves three things from one process:
 *   1. a control/monitor web page          GET  /
 *   2. a JSON API the page drives          /api/ endpoints
 *   3. an ASCOM Alpaca telescope device    /api/v1/telescope/0/...
 *
 * WHY NOT busybox httpd + CGI: it is present on the device but its CGI never
 * produced output for us, and debugging someone else's CGI plumbing over a
 * serial-less SSH link cost more than writing this. This has no dependencies
 * beyond libc, forks for long jobs, and cannot wedge the capture path.
 *
 * MIT. Deliberately single-file and dependency-free so it cross-compiles with
 * the same toolchain as the rest of the on-disk tools.
 * =========================================================================== */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ctype.h>

#include "polaris-link.h"
#include "polaris-jog.h"
#include "polaris-prog.h"
#include "polaris-astro.h"
#include "webui.h"

#define PORT_DEFAULT   8080
#define JOB_LOG        "/tmp/polaris-job.log"
#define JOB_STATUS     "/tmp/polaris-job.status"   /* idle|running|done|failed */
#define JOB_RESULT     "/tmp/polaris-job.result"   /* solver JSON */
#define JOB_PID        "/tmp/polaris-job.pid"      /* pid of the running job */
#define KEEPWIFI_FLAG  "/app/sd/polaris-astro/keep-wifi-awake"
#define KEEPWIFI_PID   "/tmp/wifi-keepalive.pid"
#define KEEPWIFI_FORCE "/app/sd/polaris-astro/keep-wifi-force"

/* Find the keepalive by scanning /proc, not by trusting the pidfile and not
 * with `ps | grep`.
 *
 * The pidfile is a hint, not the truth: a duplicate instance that starts, sees
 * the first one and exits can take the file with it, after which the page
 * reported "not running" while the helper was connected and registered. And a
 * shell `ps | grep wifi-keepalive` run from here matches the very command line
 * doing the matching -- that mistake has already killed this session's ssh
 * twice and silently disabled a start guard. /proc has neither problem. */
static pid_t find_proc(const char *needle) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t found = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        char path[64], buf[256];
        int fd, n; long pid;
        char *end;
        pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        n = (int)read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        { int i; for (i = 0; i < n - 1; i++) if (!buf[i]) buf[i] = ' '; }
        if (strstr(buf, needle)) { found = (pid_t)pid; break; }
    }
    closedir(d);
    return found;
}
#define JOB_START      "/tmp/polaris-job.start"    /* unix time it began */
#define ASTRO_DIR      "/app/astro"

static const char *g_astro = ASTRO_DIR;
/* Which mount to talk to. Defaults to the real one on this device; point it at
 * polaris-sim to run a conformance suite without commanding real motors --
 * ConformU fires dozens of slews and syncs and wedged polestar_app when aimed
 * at the hardware. */
/* goto-radec only nudges after arrival if the sky moved more than
 * --refine-arcmin, default 2 arcmin = 120 arcsec. A slew takes ~10 s, during
 * which the sky moves ~150 arcsec, so most arrivals landed just under the
 * threshold and were never corrected -- Conform measured 80-150 arcsec of RA
 * error against a 10 arcsec tolerance. We ask for 0.1 arcmin (6 arcsec). */
static const char *g_mount_host = "127.0.0.1";
static int         g_mount_port = 9090;
static double g_lat = 0.0, g_lon = 0.0;
static int    g_have_pos = 0;
static double g_focal = 400.0;

/* Manual focal override, set from the web UI and shared with the solve scripts
 * through a file so the autosolve daemon honours it too. Empty file / absent =
 * automatic (EXIF, then the cached focal, then a range search). */
#define FOCAL_OVERRIDE_FILE "/app/sd/polaris-astro/focal-override"

/* ---------------------------------------------------------------- helpers */

static double stage2_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static void set_status(const char *s) {
    int fd = open(JOB_STATUS, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) { write(fd, s, strlen(s)); close(fd); }
}

static void read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    ssize_t n = 0;
    buf[0] = 0;
    if (fd < 0) return;
    n = read(fd, buf, cap - 1);
    close(fd);
    if (n > 0) buf[n] = 0;
}

/* tail of a file, at most cap-1 bytes */
static void read_tail(const char *path, char *buf, size_t cap) {
    struct stat st;
    int fd;
    ssize_t n;
    buf[0] = 0;
    if (stat(path, &st) != 0) return;
    fd = open(path, O_RDONLY);
    if (fd < 0) return;
    if ((size_t)st.st_size > cap - 1)
        lseek(fd, st.st_size - (off_t)(cap - 1), SEEK_SET);
    n = read(fd, buf, cap - 1);
    close(fd);
    if (n > 0) buf[n] = 0;
}

/* JSON-escape into out (best effort; our strings are ASCII log lines) */
static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++]='\\'; out[o++]=c; }
        else if (c == '\n') { out[o++]='\\'; out[o++]='n'; }
        else if (c == '\r') { }
        else if (c == '\t') { out[o++]='\\'; out[o++]='t'; }
        else if (c < 0x20 || c == 0x7f) { o += snprintf(out+o, cap-o, "\\u%04x", c); }
        else out[o++] = c;
    }
    out[o] = 0;
}

/* Run a command, capture stdout (blocking, bounded).
 *
 * Returns 0 if we got ANY output, -1 otherwise -- deliberately NOT pclose()'s
 * status. pclose() cannot be trusted here: if SIGCHLD is ever SIG_IGN the child
 * is reaped by the kernel before pclose() can wait() for it and pclose returns
 * -1 even though the command ran perfectly. That exact combination silently
 * zeroed every mount reading in the first cut of this server.
 */
static int run_capture(const char *cmd, char *out, size_t cap) {
    FILE *p = popen(cmd, "r");
    size_t n;
    out[0] = 0;
    if (!p) return -1;
    n = fread(out, 1, cap - 1, p);
    out[n] = 0;
    pclose(p);
    return out[0] ? 0 : -1;
}


/* ---- passive state, straight out of the device's own log ------------------
 *
 * The device logs every 284 reply it sends the app:
 *     code[284],val[mode:8;state:0;track:1;speed:2;...]
 * Reading that file costs ZERO connections, so we can always show mode/track
 * without touching the control port. track:3 means never-aligned; it becomes 0
 * or 1 once an alignment completes.
 *
 * This is what makes "only poll when already aligned" possible: we learn the
 * alignment state for free, and only open a real connection when the mount is
 * in a state where doing so will not nag the app into re-prompting for compass
 * calibration.
 */
static int log_state(int *mode, int *track) {
    char buf[65536], *p, *last = NULL;
    int fd;
    struct stat st;
    ssize_t n;
    *mode = -1; *track = -1;
    if (stat("/app/Mlog.txt", &st) != 0) return 0;
    fd = open("/app/Mlog.txt", O_RDONLY);
    if (fd < 0) return 0;
    if ((size_t)st.st_size > sizeof buf - 1)
        lseek(fd, st.st_size - (off_t)(sizeof buf - 1), SEEK_SET);
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    /* last occurrence wins -- the log is append-ordered */
    for (p = buf; (p = strstr(p, "code[284]")) != NULL; p++) last = p;
    if (!last) return 0;
    { const char *m = strstr(last, "mode:");  if (m) *mode  = atoi(m + 5); }
    { const char *t = strstr(last, "track:"); if (t) *track = atoi(t + 6); }
    return (*mode >= 0 || *track >= 0);
}

/* Last track value we have ever observed, from the log OR from a real read.
 * Mlog.txt is truncated constantly (seen at 124 bytes), so "no 284 in the log
 * right now" is the common case and does NOT mean "unaligned" -- it means we
 * do not know yet. Remembering the last value makes the decision stable. */
static int g_last_track = -1;
#define TRACK_FILE "/tmp/polaris-track"

/* PERSIST WHAT WE LEARN. The mount's state is read out of /app/Mlog.txt, and
 * the device TRUNCATES that file -- it has been found at 0 bytes, with no 284
 * lines at all, minutes after an alignment. Anything that re-derives alignment
 * by grepping the log therefore forgets it, which is how the wifi keepalive
 * sat "waiting for the mount to be aligned" while the mount was aligned and
 * tracking. Recording it in /tmp means every component agrees, and it survives
 * a restart of this server (but not a reboot, which is correct -- alignment
 * does not survive a power cycle either). */
static void note_track(int track) {
    if (track < 0) return;
    if (track != g_last_track) {
        FILE *f = fopen(TRACK_FILE, "w");
        if (f) { fprintf(f, "%d\n", track); fclose(f); }
    }
    g_last_track = track;
}

/* read back what a previous run (or another component) recorded */
static void load_track(void) {
    FILE *f = fopen(TRACK_FILE, "r");
    int t;
    if (!f) return;
    if (fscanf(f, "%d", &t) == 1 && t >= 0) g_last_track = t;
    fclose(f);
}

/* Tri-state: 1 aligned, 0 known-unaligned, -1 unknown. */
static int alignment_state(void) {
    int mode, track;
    if (log_state(&mode, &track) && track >= 0) {
        note_track(track);
        return track != 3;
    }
    if (g_last_track >= 0) return g_last_track != 3;
    return -1;                       /* never seen any state */
}

/* May we open a connection to the mount?
 *
 * Rule (the user's): only poll when it is already aligned. Connecting while the
 * mount is unaligned is what made the Benro app re-prompt for compass
 * calibration.
 *
 * Unknown is allowed exactly once, to bootstrap: a real read sends 284, the
 * device logs its reply, and every decision after that is evidence-based. That
 * costs at most one connection ever, versus refusing forever whenever the log
 * happens to be empty -- which, given how aggressively it is truncated, would
 * be most of the time. */
static int may_read_mount(void) {
    /* UNKNOWN MEANS NO.
     *
     * This used to allow one "bootstrap" read when the alignment state was
     * unknown, so it could learn the state by asking. That was wrong in the
     * worst way: after every reboot the state IS unknown, so the first position
     * read connected to an unaligned mount -- and connecting while unaligned is
     * exactly what makes the Benro app demand a compass calibration. The user
     * hit that, I fixed it, and then reintroduced it with this bootstrap.
     *
     * Alignment state is learned PASSIVELY from the device's own 284 log lines,
     * which cost nothing. If we have never seen one, we do not know, and the
     * safe answer is to stay quiet. Position reads fall back to the last solve.
     *
     * It also removes a hang: 518 never answers on an unaligned mount, so the
     * bootstrap read waited out polaris-mount's timeout on every request. */
    return alignment_state() == 1;
}

/* ------------------------------------------------------------ mount state */

/* Ask the mount where it is.
 *
 * Two different commands, two different shapes -- getting this wrong is why the
 * first cut reported alt/az 0 and aligned false on a mount that was aligned:
 *   pose  -> {"alt_deg":28.265282,"az_deg":162.074631}
 *   state -> {"mode":8,"track":0,"aligned":true,"astro":true}
 */
static int mount_pose(double *alt, double *az, char *raw, size_t rawcap) {
    char cmd[512], buf[1024];
    const char *p;
    int ok = 0;

    /* CACHE. Every polaris-mount invocation opens a NEW TCP connection to the
     * control port, which the device treats as an app connecting
     * (SP_EVENT_APP_CONNECT) and answers by pushing a state banner. With the
     * page polling every 1.5 s that is two fresh connections per poll, and the
     * Benro app flashes its celestial-position banner continuously. Observed on
     * hardware. So hold the last reading and only re-ask occasionally. */
    static double c_alt = 0, c_az = 0, c_when = 0;
    static char   c_raw[512] = {0};
    static int    c_ok = 0;
    double now = stage2_now_ms();
    double ttl = 5000.0;                     /* ms */
    {
        const char *e = getenv("POLARIS_MOUNT_CACHE_MS");
        if (e && *e) ttl = atof(e);
    }
    if (c_when != 0.0 && (now - c_when) < ttl) {
        *alt = c_alt; *az = c_az;
        if (raw) snprintf(raw, rawcap, "%s", c_raw);
        return c_ok;
    }

    snprintf(cmd, sizeof cmd, "%s/polaris-mount --host %s --port %d pose 2>/dev/null", g_astro, g_mount_host, g_mount_port);
    if (run_capture(cmd, buf, sizeof buf) >= 0 && buf[0]) {
        p = strstr(buf, "\"alt_deg\":"); if (p) { *alt = atof(p + 10); ok = 1; }
        p = strstr(buf, "\"az_deg\":");  if (p) { *az  = atof(p + 9);  ok = 1; }
    }
    snprintf(cmd, sizeof cmd, "%s/polaris-mount --host %s --port %d state 2>/dev/null", g_astro, g_mount_host, g_mount_port);
    if (run_capture(cmd, buf, sizeof buf) >= 0) {
        snprintf(c_raw, sizeof c_raw, "%s", buf);
        if (raw) snprintf(raw, rawcap, "%s", buf);
    }
    c_alt = *alt; c_az = *az; c_ok = ok; c_when = now;
    return ok;
}

/* ------------------------------------------------------------------- jobs */

/* Fork a solve job. Parent returns immediately so the UI can poll. */
/* mode: 0 = fire our own capture, 1 = solve newest frame, 2 = wait for a new one.
 *
 * Modes 1/2 exist because astro mode refuses externally-initiated captures --
 * opcode 264 is ignored and the 272 lapse sequence is refused while tracking.
 * In astro mode the user triggers the shot in the Benro app and we follow. */
static int start_solve(int apply, int mode) {
    pid_t pid;
    char status[32];
    read_file(JOB_STATUS, status, sizeof status);
    if (strncmp(status, "running", 7) == 0) {
        /* RUNNING, or ONLY LOOKS LIKE IT. A job that died without setting a
         * final status used to hold this lock forever, and every later solve
         * was refused silently -- the page just did nothing, which reads as
         * "the solver is broken". Trust the pid, not the string. */
        char pb[32]; long jp = 0;
        read_file(JOB_PID, pb, sizeof pb);
        jp = atol(pb);
        if (jp > 0 && kill((pid_t)jp, 0) == 0) return 0;  /* genuinely running */
        set_status("failed");                              /* stale -- clear it */
        unlink(JOB_PID);
    }

    set_status("running");
    unlink(JOB_RESULT);
    { FILE *f = fopen(JOB_START, "w"); if (f) { fprintf(f, "%ld\n", (long)time(NULL)); fclose(f); } }

    pid = fork();
    if (pid < 0) { set_status("failed"); return -1; }
    if (pid > 0) { int st; waitpid(pid, &st, 0); return 1; } /* reap the middle child */

    /* ---- middle child: fork again so the job is orphaned to init ---- */
    if (fork() > 0) _exit(0);

    /* ---- grandchild: the actual job ---- */
    setsid();
    { FILE *f = fopen(JOB_PID, "w"); if (f) { fprintf(f, "%ld\n", (long)getpid()); fclose(f); } }
    {
        char cmd[1024];
        int fd, i;
        /* CLOSE THE CLIENT SOCKET WE INHERITED.
         *
         * fork() hands the job a copy of the accepted connection, so the HTTP
         * response is written and the parent closes its end -- but the socket
         * stays open in the job, the client never sees EOF, and the request
         * hangs for as long as the job runs. With a --wait job that is five
         * minutes of a browser tab spinning on a request that was actually
         * answered immediately. Close everything above stderr and give the job
         * its own stdin. */
        for (i = 3; i < 64; i++) close(i);
        fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) { dup2(fd, 0); if (fd != 0) close(fd); }
        fd = open(JOB_LOG, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        /* mode 3 (capture) passes NO flag: solve-now.sh's default path fires a
         * single shot with opcode 264 and waits for the frame. It never uses
         * the 272 lapse sequence, which wedges the camera in astro mode. */
        const char *mflag = (mode == 1) ? "--latest" : (mode == 2 ? "--wait" : "");
        /* --focal auto: let polaris-align.sh resolve it (manual override file,
         * then EXIF, then the cached focal, then a range search). Forcing
         * g_focal here would reintroduce the bug that cost a whole night --
         * a stale command-line focal overriding what the frame itself says. */
        snprintf(cmd, sizeof cmd,
                 "LAT=%.6f LON=%.6f WAIT_FOR_FRAME=300 sh %s/solve-now.sh --focal auto %s %s > %s 2>>%s",
                 g_lat, g_lon, g_astro, mflag,
                 apply ? "--apply" : "", JOB_RESULT, JOB_LOG);
        int rc = system(cmd);
        set_status(rc == 0 ? "done" : "failed");
        unlink(JOB_PID);
        _exit(0);
    }
}

/* ------------------------------------------------------------- http utils */

static void send_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) { if (errno == EINTR) continue; return; }
        off += (size_t)n;
    }
}

static void respond(int fd, int code, const char *ctype, const char *body, size_t blen) {
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"        /* Alpaca clients & the page */
        "Access-Control-Allow-Methods: GET,PUT,POST,OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n\r\n",
        code, code == 200 ? "OK" : (code == 404 ? "Not Found" : "Error"),
        ctype, blen);
    send_all(fd, hdr, (size_t)n);
    if (blen) send_all(fd, body, blen);
}

/* Alpaca requires an HTTP error status for a malformed REQUEST (bad parameter
 * value, bad parameter name casing, unknown method), as distinct from a
 * well-formed request the device cannot satisfy -- that is HTTP 200 with
 * ErrorNumber set. We were answering 200 for both. */
static void respond_400(int fd, const char *why) {
    char body[256];
    int n = snprintf(body, sizeof body, "400 Bad Request: %s\n", why);
    respond(fd, 400, "text/plain", body, (size_t)n);
}

static int parse_bool_strict(const char *v, int *out) {
    if (!strcmp(v, "True")  || !strcmp(v, "true"))  { *out = 1; return 1; }
    if (!strcmp(v, "False") || !strcmp(v, "false")) { *out = 0; return 1; }
    return 0;
}

static int parse_double_strict(const char *v, double *out) {
    char *end = NULL;
    if (!v || !*v) return 0;
    *out = strtod(v, &end);
    return end != v && *end == '\0';
}

static int parse_long_strict(const char *v, long *out) {
    char *end = NULL;
    if (!v || !*v) return 0;
    *out = strtol(v, &end, 10);
    return end != v && *end == '\0';
}

static void respond_json(int fd, const char *json) {
    respond(fd, 200, "application/json", json, strlen(json));
}

/* query/body param lookup: name=value, returns 1 if found */
/* Alpaca's casing rule is SPLIT, and getting it half right fails half the
 * suite either way:
 *   - PUT body parameters are CASE-SENSITIVE. A wrongly-cased one is a
 *     malformed request (HTTP 400). ConformU's device check requires this.
 *   - GET query parameters are CASE-INSENSITIVE. A differently-cased one must
 *     still be honoured (HTTP 200). ConformU's protocol check requires this.
 * ClientTransactionID follows the same split: echoed when it arrives in a query
 * string under any casing, exact-match only in a PUT body.
 *
 * I made everything case-sensitive after the device check and broke the
 * protocol check -- 55 issues from one over-generalised fix. */
static int param_ci(const char *qs, const char *name, char *out, size_t cap,
                    int case_sensitive) {
    size_t nl = strlen(name);
    const char *p = qs;
    out[0] = 0;
    if (!qs) return 0;
    while (*p) {
        const char *amp, *eq;
        while (*p == '&' || *p == '?') p++;
        if (!*p) break;
        amp = strchr(p, '&'); if (!amp) amp = p + strlen(p);
        eq  = memchr(p, '=', (size_t)(amp - p));
        if (eq && (size_t)(eq - p) == nl &&
            (case_sensitive ? strncmp(p, name, nl) : strncasecmp(p, name, nl)) == 0) {
            /* URL-DECODE. Bodies are application/x-www-form-urlencoded, so a
             * timestamp arrives as 2026-08-19T05%3A48%3A38Z. Returning the raw
             * text made ISO-8601 validation reject perfectly good dates -- and
             * my curl test passed only because curl sent literal colons. */
            const char *src = eq + 1;
            size_t vl = (size_t)(amp - eq - 1), i2 = 0, o2 = 0;
            for (; i2 < vl && o2 + 1 < cap; i2++) {
                if (src[i2] == '%' && i2 + 2 < vl &&
                    isxdigit((unsigned char)src[i2+1]) && isxdigit((unsigned char)src[i2+2])) {
                    char hx[3] = { src[i2+1], src[i2+2], 0 };
                    out[o2++] = (char)strtol(hx, NULL, 16);
                    i2 += 2;
                } else if (src[i2] == '+') {
                    out[o2++] = ' ';
                } else {
                    out[o2++] = src[i2];
                }
            }
            out[o2] = 0;
            return 1;
        }
        p = amp;
    }
    return 0;
}

static int param(const char *qs, const char *name, char *out, size_t cap);
static int param_ci(const char *qs, const char *name, char *out, size_t cap, int cs);
static int current_radec(double *ra, double *dec);
static int may_read_mount(void);
/* Corrected-UTC string for polaris-mount. Slews MUST use the same clock the
 * reads use: goto-radec converts RA/Dec to alt/az, so running it on the raw
 * device clock sent the mount ~105 deg (7 h) from the target while our position
 * reads were correct -- ConformU saw "Slewed 379127 arc seconds away". Third
 * place this same clock bug has surfaced. */
/* --clock-offset, NOT --utc.
 *
 * --utc freezes an instant (for replaying a past capture) and deliberately
 * disables goto arrival refinement. Passing it on every call -- which is what
 * we did to work around the Polaris' local-time clock -- silently disabled
 * refinement everywhere, leaving ~100 arcsec of RA error after every goto
 * because the sky moves during the ~10 s slew and nothing corrected for it.
 *
 * --clock-offset corrects the clock while letting time keep advancing, which
 * is what a wrong clock actually needs, and lets refinement work. */
static void utc_arg(char *out, size_t cap) {
    extern long tz_offset_sec(void);
    snprintf(out, cap, "--clock-offset %ld", tz_offset_sec());
}

/* Query-string lookup: case-INSENSITIVE (GET rule). */
static int param(const char *qs, const char *name, char *out, size_t cap) {
    return param_ci(qs, name, out, cap, 0);
}
/* Body lookup: case-SENSITIVE (PUT rule). */
static int param_body(const char *b, const char *name, char *out, size_t cap) {
    return param_ci(b, name, out, cap, 1);
}

/* ----------------------------------------------------------------- Alpaca */
/*
 * ASCOM Alpaca telescope device. Enough for Stellarium / NINA / SkySafari to
 * see where the mount is pointing and to slew it.
 *
 * Alpaca conventions that bite if you get them wrong:
 *   - RightAscension is in HOURS, not degrees. Declination is degrees.
 *   - Every response carries ClientTransactionID / ServerTransactionID.
 *   - Errors go in ErrorNumber/ErrorMessage with HTTP 200, not an HTTP error.
 */
static unsigned long g_server_txn = 0;
/* Alpaca PUTs carry their parameters -- ClientTransactionID included -- in the
 * request BODY, not the query string. handle_alpaca() parks it here so the
 * envelope builders can find it without threading it through every call. */
static const char *g_req_body = NULL;

/* ClientTransactionID from query string, falling back to the PUT body. */
static long client_txn(const char *qs) {
    char cid[32];
    long v;
    char *end = NULL;
    const char *src = NULL;
    if (param(qs, "ClientTransactionID", cid, sizeof cid)) src = cid;
    else if (g_req_body && param(g_req_body, "ClientTransactionID", cid, sizeof cid)) src = cid;
    if (!src) return 0;
    v = strtol(cid, &end, 10);
    /* Must be a valid UNSIGNED integer. A negative or unparseable value is
     * treated as absent and echoed as 0 -- echoing it back produces JSON the
     * client cannot parse into its unsigned field. */
    if (end == cid || *end != '\0' || v < 0) return 0;
    return v;
}
static double g_target_ra = 0.0, g_target_dec = 0.0;
static int    g_target_set = 0;
static int    g_connected = 1;
/* Slewing must read True while an async slew is in flight -- Conform checks it
 * immediately after SlewToCoordinatesAsync returns. We fork the slew and treat
 * "child still alive" as slewing, which is honest: the goto really is running. */
static pid_t  g_slew_pid = -1;
static double g_site_elev = 0.0;

/* THE DEVICE CLOCK IS NOT UTC. It runs local time while reporting itself as
 * UTC; ConformU caught this as "Scope and ASCOM sidereal times are more than 1
 * hour apart" -- 7 hours, i.e. the whole PDT offset. The autosolve daemon was
 * fixed for this; polaris-httpd was not, so every Alpaca client saw wrong
 * positions. Offset is the app's own value, cached by the daemon. */
long tz_offset_sec(void) {
    static long cached = -1;
    char buf[64];
    if (cached >= 0) return cached;
    cached = 0;
    { const char *e = getenv("TZ_OFFSET_SEC");
      if (e && *e) { cached = atol(e); return cached; } }
    read_file("/app/sd/polaris-astro/tz.cache", buf, sizeof buf);
    if (buf[0]) cached = atol(buf);
    return cached;
}
time_t utc_now(void) { return time(NULL) + tz_offset_sec(); }

static void alpaca_value(int fd, const char *qs, const char *value_json) {
    char out[1024];
    long c = client_txn(qs);
    snprintf(out, sizeof out,
             "{\"Value\":%s,\"ClientTransactionID\":%ld,\"ServerTransactionID\":%lu,"
             "\"ErrorNumber\":0,\"ErrorMessage\":\"\"}",
             value_json, c, ++g_server_txn);
    respond_json(fd, out);
}

static void alpaca_error(int fd, const char *qs, int num, const char *msg) {
    char out[1024], esc[512];
    long c = client_txn(qs);
    json_escape(msg, esc, sizeof esc);
    snprintf(out, sizeof out,
             "{\"Value\":0,\"ClientTransactionID\":%ld,\"ServerTransactionID\":%lu,"
             "\"ErrorNumber\":%d,\"ErrorMessage\":\"%s\"}",
             c, ++g_server_txn, num, esc);
    respond_json(fd, out);
}

/* Last solved position, if we have one. RA degrees / Dec degrees. */
static int last_solution(double *ra_deg, double *dec_deg) {
    char buf[4096];
    const char *p;
    read_file(JOB_RESULT, buf, sizeof buf);
    if (!strstr(buf, "\"solved\":true")) return 0;
    p = strstr(buf, "\"ra_deg\":");  if (!p) return 0; *ra_deg  = atof(p + 9);
    p = strstr(buf, "\"dec_deg\":"); if (!p) return 0; *dec_deg = atof(p + 10);
    return 1;
}

/* returns 1 if handled */
static int handle_alpaca(int fd, const char *method, const char *path, const char *qs,
                         const char *body) {
    const char *m;
    char v[128];
    g_req_body = body;

    if (strncmp(path, "/management/", 12) == 0) {
        if (strstr(path, "apiversions"))
            { alpaca_value(fd, qs, "[1]"); return 1; }
        if (strstr(path, "/v1/description")) {
            alpaca_value(fd, qs,
                "{\"ServerName\":\"Benro Polaris plate solver\",\"Manufacturer\":\"polaris-httpd\","
                "\"ManufacturerVersion\":\"1.0\",\"Location\":\"on-device\"}");
            return 1;
        }
        if (strstr(path, "configureddevices")) {
            alpaca_value(fd, qs,
                "[{\"DeviceName\":\"Benro Polaris\",\"DeviceType\":\"Telescope\","
                "\"DeviceNumber\":0,\"UniqueID\":\"benro-polaris-0\"}]");
            return 1;
        }
        alpaca_error(fd, qs, 1025, "unknown management call");
        return 1;
    }

    if (strncmp(path, "/api/v1/telescope/0/", 20) != 0) return 0;
    m = path + 20;

    /* ---- validate the REQUEST before answering it ------------------------
     *
     * ConformU sends deliberately bad values (empty string, "abcd", wrong
     * name casing) and requires HTTP 400. Returning 200 for these was 40 of
     * the 44 issues it found. Each entry names the parameter the spec defines
     * and how it must parse; a value that does not is a malformed request. */
    if (!strcmp(method, "PUT")) {
        static const struct { const char *meth, *param; char type; } P[] = {
            { "connected",                "Connected",               'b' },
            { "tracking",                 "Tracking",                'b' },
            { "doesrefraction",           "DoesRefraction",          'b' },
            { "declinationrate",          "DeclinationRate",         'd' },
            { "rightascensionrate",       "RightAscensionRate",      'd' },
            { "guideratedeclination",     "GuideRateDeclination",    'd' },
            { "guideraterightascension",  "GuideRateRightAscension", 'd' },
            { "siteelevation",            "SiteElevation",           'd' },
            { "sitelatitude",             "SiteLatitude",            'd' },
            { "sitelongitude",            "SiteLongitude",           'd' },
            { "slewsettletime",           "SlewSettleTime",          'i' },
            { "sideofpier",               "SideOfPier",              'i' },
            { "trackingrate",             "TrackingRate",            'i' },
            { "targetrightascension",     "TargetRightAscension",    'd' },
            { "targetdeclination",        "TargetDeclination",       'd' },
            { "utcdate",                  "UTCDate",                 's' },
            { NULL, NULL, 0 }
        };
        int i;
        for (i = 0; P[i].meth; i++) {
            if (strcmp(m, P[i].meth)) continue;
            {
                const char *src = (body && *body) ? body : qs;
            /* PUT: body params are case-sensitive (see param_ci) */
                char v2[128];
                double dv; long lv; int bv;
                if (!(body && *body ? param_body(src, P[i].param, v2, sizeof v2)
                                    : param(src, P[i].param, v2, sizeof v2))) {
                    respond_400(fd, "required parameter missing or wrongly cased");
                    return 1;
                }
                switch (P[i].type) {
                    case 'b': if (!parse_bool_strict(v2, &bv))   { respond_400(fd, "not a boolean"); return 1; } break;
                    case 'd': if (!parse_double_strict(v2, &dv)) { respond_400(fd, "not a number");  return 1; } break;
                    case 'i': if (!parse_long_strict(v2, &lv))   { respond_400(fd, "not an integer");return 1; } break;
                    case 's': {
                        /* UTCDate must be a real ISO-8601 instant. Checking only
                         * for non-empty let garbage through with HTTP 200. */
                        int Y, Mo, D, h, mi; double se;
                        if (sscanf(v2, "%d-%d-%dT%d:%d:%lf", &Y, &Mo, &D, &h, &mi, &se) < 6 ||
                            Y < 1900 || Mo < 1 || Mo > 12 || D < 1 || D > 31 ||
                            h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se >= 61) {
                            respond_400(fd, "UTCDate is not an ISO-8601 instant");
                            return 1;
                        }
                        break;
                    }
                }
            }
            break;
        }
        /* coordinate setters: both parameters, both numeric, both exact case */
        if (!strcmp(m, "slewtocoordinates") || !strcmp(m, "slewtocoordinatesasync") ||
            !strcmp(m, "synctocoordinates") || !strcmp(m, "slewtoaltaz") ||
            !strcmp(m, "slewtoaltazasync")  || !strcmp(m, "synctoaltaz")) {
            const char *src = (body && *body) ? body : qs;
            /* PUT: body params are case-sensitive (see param_ci) */
            const char *a = (strstr(m, "altaz")) ? "Azimuth"  : "RightAscension";
            const char *b = (strstr(m, "altaz")) ? "Altitude" : "Declination";
            char v2[128]; double dv;
            if (!(body && *body ? param_body(src, a, v2, sizeof v2) : param(src, a, v2, sizeof v2))
                || !parse_double_strict(v2, &dv)) {
                respond_400(fd, "bad or missing coordinate parameter"); return 1;
            }
            if (!(body && *body ? param_body(src, b, v2, sizeof v2) : param(src, b, v2, sizeof v2))
                || !parse_double_strict(v2, &dv)) {
                respond_400(fd, "bad or missing coordinate parameter"); return 1;
            }
        }
    } else {
        /* GETs that take parameters must validate them too */
        char v2[128]; long lv; double dv;
        if (!strcmp(m, "axisrates") || !strcmp(m, "canmoveaxis")) {
            if (!param(qs, "Axis", v2, sizeof v2) || !parse_long_strict(v2, &lv) ||
                lv < 0 || lv > 2) { respond_400(fd, "bad Axis"); return 1; }
        }
        if (!strcmp(m, "destinationsideofpier")) {
            if (!param(qs, "RightAscension", v2, sizeof v2) || !parse_double_strict(v2, &dv) ||
                !param(qs, "Declination",    v2, sizeof v2) || !parse_double_strict(v2, &dv)) {
                respond_400(fd, "bad coordinate parameter"); return 1;
            }
        }
    }

    /* --- capability / static properties --- */
    if (!strcmp(m, "interfaceversion")) { alpaca_value(fd, qs, "3");     return 1; }
    if (!strcmp(m, "name"))        { alpaca_value(fd, qs, "\"Benro Polaris\""); return 1; }
    if (!strcmp(m, "description")) { alpaca_value(fd, qs, "\"Benro Polaris via on-device plate solver\""); return 1; }
    if (!strcmp(m, "driverinfo"))  { alpaca_value(fd, qs, "\"polaris-httpd\""); return 1; }
    if (!strcmp(m, "driverversion")) { alpaca_value(fd, qs, "\"1.0\""); return 1; }
    if (!strcmp(m, "supportedactions")) { alpaca_value(fd, qs, "[]"); return 1; }
    if (!strcmp(m, "alignmentmode"))    { alpaca_value(fd, qs, "1"); return 1; } /* altaz */
    if (!strcmp(m, "equatorialsystem")) { alpaca_value(fd, qs, "2"); return 1; } /* J2000 */
    if (!strcmp(m, "cansetpark") || !strcmp(m, "canpark") ||
        !strcmp(m, "canfindhome") || !strcmp(m, "canpulseguide") ||
        !strcmp(m, "cansettracking") || !strcmp(m, "cansetpierside") ||
        !strcmp(m, "cansetguiderates") || !strcmp(m, "canslewaltaz") ||
        !strcmp(m, "canslewaltazasync") || !strcmp(m, "cansyncaltaz") ||
        !strcmp(m, "canmoveaxis") || !strcmp(m, "canunpark") ||
        !strcmp(m, "cansetdeclinationrate") || !strcmp(m, "cansetrightascensionrate"))
        { alpaca_value(fd, qs, "false"); return 1; }
    if (!strcmp(m, "canslew") || !strcmp(m, "canslewasync"))
        { alpaca_value(fd, qs, "true"); return 1; }
    /* CanSync is TRUE. A real two-axis sync IS possible: the 530 sequence
     * (star-align) declares the mount is looking at a given alt/az in BOTH
     * axes, which is exactly ASCOM's contract. An earlier version reported
     * false because I looked at `align` (compass/527, azimuth-only) and never
     * checked `star-align`.
     *
     * HAZARD, honoured with a cooldown: repeated 530s are documented to wedge
     * the motors ("send it ONCE" -- field traces from the Aperion work), and
     * the first ConformU run left polestar_app alive but no longer listening.
     * So syncs are rate-limited below. A refused sync returns a clear error
     * rather than silently doing nothing. */
    if (!strcmp(m, "cansync")) { alpaca_value(fd, qs, "true"); return 1; }

    if (!strcmp(m, "atpark") || !strcmp(m, "athome"))
        { alpaca_value(fd, qs, "false"); return 1; }
    /* NOT ispulseguiding: CanPulseGuide is False, so it must raise
     * NotImplemented. It used to be answered "false" by this branch, which sat
     * ABOVE the check that was supposed to reject it -- so the fix never ran. */
    if (!strcmp(m, "sideofpier")) { alpaca_value(fd, qs, "-1"); return 1; }

    if (!strcmp(m, "connected")) {
        if (!strcmp(method, "PUT")) {
            const char *src = (body && *body) ? body : qs;
            /* PUT: body params are case-sensitive (see param_ci) */
            char v2[32]; int bv;
            if (!(body && *body ? param_body(src, "Connected", v2, sizeof v2) : param(src, "Connected", v2, sizeof v2))
                || !parse_bool_strict(v2, &bv)) {
                respond_400(fd, "bad Connected value"); return 1;
            }
            /* A fresh connection is a fresh session: the target must be unset
             * again, or "read before write" wrongly succeeds with a stale value
             * left by the previous client. g_target_set otherwise lives for the
             * life of the process. */
            /* Reset on ANY Connected=True, not only a false->true edge.
             * Conform connects while g_connected is already 1 (it survives from
             * a previous client), so the edge never fired and "read before
             * write" kept succeeding with a stale target. */
            if (bv) g_target_set = 0;
            g_connected = bv;          /* must actually stick: Conform sets it False and reads back */
            alpaca_value(fd, qs, "null"); return 1;
        }
        alpaca_value(fd, qs, g_connected ? "true" : "false"); return 1;
    }
    /* Site properties must range-check AND round-trip. We were ignoring writes
     * and returning the configured value, which ConformU catches twice: no
     * error on an out-of-range value, and the test value not coming back. */
    if (!strcmp(m, "sitelatitude") || !strcmp(m, "sitelongitude") ||
        !strcmp(m, "siteelevation")) {
        double lo = -90, hi = 90, *slot = &g_lat;
        const char *pn = "SiteLatitude";
        char b[64];
        if (!strcmp(m, "sitelongitude")) { lo=-180; hi=180; slot=&g_lon; pn="SiteLongitude"; }
        if (!strcmp(m, "siteelevation")) { lo=-300; hi=10000; slot=&g_site_elev; pn="SiteElevation"; }
        if (!strcmp(method, "PUT")) {
            const char *src = (body && *body) ? body : qs;
            /* PUT: body params are case-sensitive (see param_ci) */
            char v2[64]; double dv;
            if (!param(src, pn, v2, sizeof v2) || !parse_double_strict(v2, &dv)) {
                respond_400(fd, "bad value"); return 1;
            }
            if (dv < lo || dv > hi) { alpaca_error(fd, qs, 1025, "value out of range"); return 1; }
            *slot = dv;
            alpaca_value(fd, qs, "null"); return 1;
        }
        snprintf(b, sizeof b, "%.6f", *slot);
        alpaca_value(fd, qs, b); return 1;
    }
    if (!strcmp(m, "slewsettletime")) {
        if (!strcmp(method, "PUT")) {
            const char *src = (body && *body) ? body : qs;
            /* PUT: body params are case-sensitive (see param_ci) */
            char v2[64]; long lv;
            if (!param(src, "SlewSettleTime", v2, sizeof v2) || !parse_long_strict(v2, &lv)) {
                respond_400(fd, "bad value"); return 1;
            }
            if (lv < 0) { alpaca_error(fd, qs, 1025, "SlewSettleTime cannot be negative"); return 1; }
            alpaca_value(fd, qs, "null"); return 1;
        }
        alpaca_value(fd, qs, "0"); return 1;
    }
    if (!strcmp(m, "doesrefraction")) { alpaca_value(fd, qs, "false"); return 1; }
    if (!strcmp(m, "focallength")) {
        char b[64]; snprintf(b, sizeof b, "%.4f", g_focal / 1000.0);   /* metres */
        alpaca_value(fd, qs, b); return 1;
    }
    /* Writing a property whose corresponding Can* is False must raise
     * NotImplemented, not silently succeed -- ConformU flags each one. */
    if (!strcmp(method, "PUT") &&
        (!strcmp(m, "declinationrate") || !strcmp(m, "rightascensionrate") ||
         !strcmp(m, "guideratedeclination") || !strcmp(m, "guideraterightascension") ||
         !strcmp(m, "sideofpier") || !strcmp(m, "trackingrate") ||
         !strcmp(m, "tracking"))) {
        alpaca_error(fd, qs, 1024, "not implemented: the matching Can property is False");
        return 1;
    }
    if (!strcmp(m, "ispulseguiding")) {
        alpaca_error(fd, qs, 1024, "not implemented: CanPulseGuide is False");
        return 1;
    }
    if (!strcmp(m, "trackingrate"))  { alpaca_value(fd, qs, "0"); return 1; }   /* sidereal */
    if (!strcmp(m, "trackingrates")) { alpaca_value(fd, qs, "[0]"); return 1; }
    if (!strcmp(m, "axisrates"))     { alpaca_value(fd, qs, "[]"); return 1; }
    if (!strcmp(m, "declinationrate") || !strcmp(m, "rightascensionrate") ||
        !strcmp(m, "guideratedeclination") || !strcmp(m, "guideraterightascension"))
        { alpaca_value(fd, qs, "0"); return 1; }
    if (!strcmp(m, "aperturearea") || !strcmp(m, "aperturediameter"))
        { alpaca_value(fd, qs, "0"); return 1; }

    /* Altitude/Azimuth: we genuinely have these from the mount.
     *
     * GATED, and this one was the hole. RightAscension/Declination check
     * may_read_mount(); these two did not, so ANY Alpaca client that polls
     * position -- which is the first thing they all do on connect -- opened a
     * connection to the control port per request while the mount was
     * unaligned, and that is precisely what makes the Benro app demand a
     * compass calibration. It needs no action from us at all: discovery
     * advertises the device on UDP 32227, a running ConformU/NINA/Stellarium
     * finds it by itself and starts polling. A compass prompt appearing in
     * single-photo mode, with nobody touching this box, is explained by
     * exactly this path. */
    if (!strcmp(m, "altitude") || !strcmp(m, "azimuth")) {
        double alt = 0, az = 0; char raw[512], b[64];
        if (!may_read_mount()) {
            alpaca_error(fd, qs, 1024,
                         "mount is not aligned: reading it now would make the "
                         "Benro app demand a compass calibration");
            return 1;
        }
        mount_pose(&alt, &az, raw, sizeof raw);
        snprintf(b, sizeof b, "%.6f", !strcmp(m, "altitude") ? alt : az);
        alpaca_value(fd, qs, b); return 1;
    }

    /* SiderealTime: polaris-mount reports lst_deg; Alpaca wants HOURS. */
    if (!strcmp(m, "siderealtime")) {
        char cmd[512], out[512], b[64];
        double lst = 0;
        {   char ub[64];
            snprintf(ub, sizeof ub, "--clock-offset %ld", tz_offset_sec());
            snprintf(cmd, sizeof cmd,
                 /* radec2altaz reports lst_deg; altaz2radec does NOT. */
                 "%s/polaris-mount --lat %.6f --lon %.6f %s radec2altaz --ra 0 --dec 0 2>/dev/null",
                 g_astro, g_lat, g_lon, ub);
        }
        if (run_capture(cmd, out, sizeof out) == 0) {
            const char *p2 = strstr(out, "\"lst_deg\":");
            if (p2) lst = atof(p2 + 10);
        }
        snprintf(b, sizeof b, "%.6f", lst / 15.0);
        alpaca_value(fd, qs, b); return 1;
    }

    /* Target coordinates: stored, and used by slewtotarget. */
    if (!strcmp(m, "targetrightascension") || !strcmp(m, "targetdeclination")) {
        char b[64];
        if (!strcmp(method, "PUT")) {
            const char *src = (body && *body) ? body : qs;
            /* PUT: body params are case-sensitive (see param_ci) */
            char v2[64];
            const char *want = !strcmp(m, "targetrightascension")
                             ? "TargetRightAscension" : "TargetDeclination";
            double dv;
            if (!param(src, want, v2, sizeof v2) || !parse_double_strict(v2, &dv)) {
                respond_400(fd, "bad target value"); return 1;
            }
            if (!strcmp(m, "targetrightascension")) {
                if (dv < 0.0 || dv >= 24.0) {         /* HOURS */
                    alpaca_error(fd, qs, 1025, "TargetRightAscension must be 0..24 hours");
                    return 1;
                }
                g_target_ra = dv * 15.0;
            } else {
                if (dv < -90.0 || dv > 90.0) {
                    alpaca_error(fd, qs, 1025, "TargetDeclination must be -90..90 degrees");
                    return 1;
                }
                g_target_dec = dv;
            }
            g_target_set = 1;
            alpaca_value(fd, qs, "null"); return 1;
        }
        if (!strcmp(m, "targetrightascension")) {
            if (!g_target_set) { alpaca_error(fd, qs, 1026, "target not set"); return 1; }
            snprintf(b, sizeof b, "%.6f", g_target_ra / 15.0);
        } else {
            if (!g_target_set) { alpaca_error(fd, qs, 1026, "target not set"); return 1; }
            snprintf(b, sizeof b, "%.6f", g_target_dec);
        }
        alpaca_value(fd, qs, b); return 1;
    }
    if (!strcmp(m, "destinationsideofpier")) { alpaca_value(fd, qs, "-1"); return 1; }

    /* --- where are we pointing? --- */
    if (!strcmp(m, "rightascension") || !strcmp(m, "declination")) {
        double ra = 0, dec = 0;
        char b[64];
        /* current_radec falls back to the last solve when the mount cannot be
         * read. With no solve yet it has NOTHING, and answering 0/0 would put
         * the telescope at the vernal equinox in every planetarium app -- the
         * exact bug the LX200 side was fixed for. Say so instead. */
        if (!current_radec(&ra, &dec)) {
            alpaca_error(fd, qs, 1024,
                         "position unknown: mount is not aligned and nothing "
                         "has been plate-solved yet");
            return 1;
        }
        if (!strcmp(m, "rightascension")) snprintf(b, sizeof b, "%.6f", ra / 15.0); /* HOURS */
        else                              snprintf(b, sizeof b, "%.6f", dec);
        alpaca_value(fd, qs, b);
        return 1;
    }
    if (!strcmp(m, "slewing")) {
        int slewing = 0;
        if (g_slew_pid > 0) {
            int st2;
            pid_t r = waitpid(g_slew_pid, &st2, WNOHANG);
            if (r == 0) slewing = 1;            /* still running */
            else g_slew_pid = -1;
        }
        alpaca_value(fd, qs, slewing ? "true" : "false");
        return 1;
    }
    if (!strcmp(m, "tracking")) { alpaca_value(fd, qs, "true"); return 1; }
    if (!strcmp(m, "utcdate")) {
        char b[64]; time_t t = utc_now(); struct tm g;
        gmtime_r(&t, &g);
        snprintf(b, sizeof b, "\"%04d-%02d-%02dT%02d:%02d:%02dZ\"",
                 g.tm_year+1900, g.tm_mon+1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
        alpaca_value(fd, qs, b); return 1;
    }

    /* --- slew / sync --- */
    if (!strcmp(m, "synctocoordinates") || !strcmp(m, "synctotarget")) {
        static time_t last_sync = 0;
        const char *src = (body && *body) ? body : qs;
            /* PUT: body params are case-sensitive (see param_ci) */
        char v2[64], cmd[640], out[1024], ua[64];
        double sra, sdec, alt = 0, az = 0;
        time_t now = time(NULL);
        int cooldown = 30;
        { const char *e = getenv("SYNC_COOLDOWN_SEC"); if (e && *e) cooldown = atoi(e); }

        if (!strcmp(m, "synctotarget")) {
            if (!g_target_set) { alpaca_error(fd, qs, 1026, "target not set"); return 1; }
            sra = g_target_ra; sdec = g_target_dec;
        } else {
            double dv;
            if (!param(src, "RightAscension", v2, sizeof v2) || !parse_double_strict(v2, &dv)) {
                respond_400(fd, "bad RightAscension"); return 1;
            }
            if (dv < 0 || dv >= 24) { alpaca_error(fd, qs, 1025, "RA must be 0..24 h"); return 1; }
            sra = dv * 15.0;
            if (!param(src, "Declination", v2, sizeof v2) || !parse_double_strict(v2, &dv)) {
                respond_400(fd, "bad Declination"); return 1;
            }
            if (dv < -90 || dv > 90) { alpaca_error(fd, qs, 1025, "Dec must be -90..90"); return 1; }
            sdec = dv;
        }

        /* Repeated 530s wedge the motors. Refuse rather than risk the mount. */
        if (last_sync && (now - last_sync) < cooldown) {
            alpaca_error(fd, qs, 1025,
                "sync refused: repeated 530 alignment commands can wedge this "
                "mount's motors, so syncs are rate limited");
            return 1;
        }

        /* RA/Dec -> alt/az, then the 530 pair, which syncs BOTH axes. */
        utc_arg(ua, sizeof ua);
        snprintf(cmd, sizeof cmd,
            "%s/polaris-mount --lat %.6f --lon %.6f %s radec2altaz --ra %.6f --dec %.6f 2>/dev/null",
            g_astro, g_lat, g_lon, ua, sra, sdec);
        if (run_capture(cmd, out, sizeof out) == 0) {
            const char *q2 = strstr(out, "\"alt_deg\":");
            if (q2) alt = atof(q2 + 10);
            q2 = strstr(out, "\"az_deg\":");
            if (q2) az = atof(q2 + 9);
        }
        if (alt <= 0) { alpaca_error(fd, qs, 1025, "target is below the horizon"); return 1; }

        snprintf(cmd, sizeof cmd,
            "%s/polaris-mount --host %s --port %d --lat %.6f --lon %.6f %s star-align --alt %.6f --az %.6f 2>&1",
            g_astro, g_mount_host, g_mount_port, g_lat, g_lon, ua, alt, az);
        run_capture(cmd, out, sizeof out);
        /* Let the mount apply the new model before we answer.
         *
         * A sync IS applied correctly -- told it it was 1.0 deg away, its
         * reported azimuth shifted +0.9983 deg -- but not instantly. Clients
         * read the position immediately after the call returns, and Conform saw
         * "synced to a position 3618 arcsec away" purely because it looked
         * before the mount had updated. Returning early is lying about when the
         * work finished. */
        { const char *e = getenv("SYNC_SETTLE_MS");
          long ms = (e && *e) ? atol(e) : 1500;
          if (ms > 0) usleep((useconds_t)(ms * 1000)); }
        last_sync = now;
        g_target_ra = sra; g_target_dec = sdec; g_target_set = 1;
        alpaca_value(fd, qs, "null");
        return 1;
    }
    if (!strcmp(m, "slewtocoordinates") || !strcmp(m, "slewtocoordinatesasync")) {
        char ra_s[64], dec_s[64], cmd[512], out[1024];
        const char *src = (body && *body) ? body : qs;
            /* PUT: body params are case-sensitive (see param_ci) */
        if (!param(src, "RightAscension", ra_s, sizeof ra_s) ||
            !param(src, "Declination", dec_s, sizeof dec_s)) {
            alpaca_error(fd, qs, 1025, "RightAscension/Declination required");
            return 1;
        }
        double ra_deg, dec_deg;
        char ua[64];
        char *endp = NULL;
        utc_arg(ua, sizeof ua);
        ra_deg = strtod(ra_s, &endp) * 15.0;          /* hours -> degrees */
        if (endp == ra_s) { alpaca_error(fd, qs, 1025, "RightAscension is not a number"); return 1; }
        endp = NULL;
        dec_deg = strtod(dec_s, &endp);
        if (endp == dec_s) { alpaca_error(fd, qs, 1025, "Declination is not a number"); return 1; }
        if (ra_deg < 0 || ra_deg >= 360 || dec_deg < -90 || dec_deg > 90) {
            alpaca_error(fd, qs, 1025, "coordinates out of range"); return 1;
        }
        if (!strcmp(m, "synctocoordinates"))
            snprintf(cmd, sizeof cmd,
                     "%s/polaris-mount --host %s --port %d --lat %.6f --lon %.6f %s align --solved-ra %.6f --solved-dec %.6f 2>&1",
                     g_astro, g_mount_host, g_mount_port, g_lat, g_lon, ua, ra_deg, dec_deg);
        else
            snprintf(cmd, sizeof cmd,
                     "%s/polaris-mount --host %s --port %d --lat %.6f --lon %.6f %s goto-radec --refine-arcmin 0.1 --ra %.6f --dec %.6f 2>&1",
                     g_astro, g_mount_host, g_mount_port, g_lat, g_lon, ua, ra_deg, dec_deg);
        /* The spec REQUIRES these to set the target properties; Conform reads
         * them back and got ValueNotSet. (My earlier edit for this silently
         * failed to apply -- I used replace() without asserting a match.) */
        g_target_ra = ra_deg; g_target_dec = dec_deg; g_target_set = 1;

        if (!strcmp(m, "slewtocoordinatesasync")) {
            /* Async MUST return immediately and leave Slewing true while the
             * goto runs. Running it inline took 8.2 s and reported Slewing
             * false, failing both the timing target and the state check. */
            pid_t pid = fork();
            if (pid == 0) { char o2[1024]; run_capture(cmd, o2, sizeof o2); _exit(0); }
            if (pid > 0) g_slew_pid = pid;
            else run_capture(cmd, out, sizeof out);   /* fork failed: do it inline */
        } else {
            run_capture(cmd, out, sizeof out);
        }
        alpaca_value(fd, qs, "null");
        return 1;
    }
    /* Mandatory whenever CanSlew / CanSync report True. Returning
     * NotImplemented for these while advertising the capability is a spec
     * violation, and ConformU flags every variant. They act on the stored
     * target, which is why TargetRA/Dec must be settable. */
    if (!strcmp(m, "slewtotarget") || !strcmp(m, "slewtotargetasync")) {
        char cmd[640], out[1024], ua[64];
        utc_arg(ua, sizeof ua);
        if (!g_target_set) {
            alpaca_error(fd, qs, 1026, "target not set");   /* ValueNotSet */
            return 1;
        }
        snprintf(cmd, sizeof cmd,
                     "%s/polaris-mount --host %s --port %d --lat %.6f --lon %.6f %s goto-radec --refine-arcmin 0.1 --ra %.6f --dec %.6f 2>&1",
                     g_astro, g_mount_host, g_mount_port, g_lat, g_lon, ua, g_target_ra, g_target_dec);
        if (!strcmp(m, "slewtotargetasync")) {
            pid_t pid = fork();
            if (pid == 0) { char o2[1024]; run_capture(cmd, o2, sizeof o2); _exit(0); }
            if (pid > 0) g_slew_pid = pid;
            else run_capture(cmd, out, sizeof out);
        } else {
            run_capture(cmd, out, sizeof out);
        }
        alpaca_value(fd, qs, "null");
        return 1;
    }

    if (!strcmp(m, "abortslew")) {
        char cmd[256], out[512];
        if (g_slew_pid > 0) { kill(g_slew_pid, SIGTERM); g_slew_pid = -1; }
        snprintf(cmd, sizeof cmd, "%s/polaris-mount --host %s --port %d abort 2>&1", g_astro, g_mount_host, g_mount_port);
        run_capture(cmd, out, sizeof out);
        alpaca_value(fd, qs, "null");
        return 1;
    }

    /* VALID Alpaca members we do not implement must answer 200 with
     * ErrorNumber 1024 -- a client checks Can* and then expects a proper
     * NotImplemented, not an HTTP error. Returning 400 for these (my previous
     * over-correction) produced nine "Unexpected error" issues. Only a name
     * that is not in the spec at all is a malformed URL. */
    {
        static const char *known[] = {
            "park", "unpark", "setpark", "findhome", "moveaxis", "pulseguide",
            "slewtoaltaz", "slewtoaltazasync", "synctoaltaz",
            "action", "commandblind", "commandbool", "commandstring", "dispose",
            "devicestate", "connect", "disconnect", "connecting", NULL
        };
        int k;
        for (k = 0; known[k]; k++)
            if (!strcmp(m, known[k])) {
                alpaca_error(fd, qs, 1024, "not implemented");
                return 1;
            }
    }
    (void)v;
    respond_400(fd, "unknown method name");
    return 1;
}


/* Where is the mount pointing, right now?
 *
 * ONE implementation, shared by Alpaca and LX200. The first LX200 cut read only
 * the last plate-solve and reported RA 0 / Dec 0 until something solved, which
 * would draw the telescope at the vernal equinox in any planetarium app. Two
 * protocols answering the same question must not have two answers.
 *
 * Mount first (that is where it IS), last solve as a fallback. */
static int current_radec(double *ra, double *dec) {
    char cmd[512], out[512], raw[512], ub[64];
    double alt = 0, az = 0;
    *ra = 0; *dec = 0;
    snprintf(ub, sizeof ub, "--clock-offset %ld", tz_offset_sec());
    /* Do not ask an UNALIGNED mount where it is pointing: 518 never answers in
     * that state and polaris-mount waits out its timeout, so a client polling
     * position before the user has calibrated simply hangs. Fall back to the
     * last solve, which is the honest answer for "we cannot tell". */
    if (may_read_mount() && mount_pose(&alt, &az, raw, sizeof raw)) {
        snprintf(cmd, sizeof cmd,
            "%s/polaris-mount --lat %.6f --lon %.6f %s altaz2radec --alt %.6f --az %.6f 2>/dev/null",
            g_astro, g_lat, g_lon, ub, alt, az);
        if (run_capture(cmd, out, sizeof out) == 0) {
            const char *q2 = strstr(out, "\"ra_deg\":");
            if (q2) {
                *ra = atof(q2 + 9);
                q2 = strstr(out, "\"dec_deg\":");
                if (q2) *dec = atof(q2 + 10);
                return 1;
            }
        }
    }
    return last_solution(ra, dec);
}

/* ===========================================================================
 * LX200 over TCP
 *
 * WHY: Alpaca is not universally supported. Stellarium on macOS has NO Alpaca
 * support at all (verified: zero matches in the 26.2.0 binary) -- it speaks
 * INDI, RTS2, LX200 or its own protocol. LX200 is the smallest thing that
 * reaches Stellarium, SkySafari, KStars and most planetarium apps, and it is a
 * plain ASCII protocol, so it costs far less than an INDI driver.
 *
 * Implemented subset -- what those clients actually send:
 *     :GR#   get RA            -> "HH:MM:SS#"
 *     :GD#   get Dec           -> "sDD*MM:SS#"
 *     :Sr..# set target RA     -> "1" ok, "0" bad
 *     :Sd..# set target Dec    -> "1" ok, "0" bad
 *     :MS#   slew to target    -> "0" ok, "1<reason>#" refused
 *     :CM#   sync to target    -> "<text>#"
 *     :Q#    abort slew        -> (no reply)
 *     :GVP# :GVN# :GVD# :GVT#  product / firmware identification
 *     :Gt# :Gg#                site latitude / longitude
 *     :U#                      toggle precision (accepted, ignored)
 *
 * Each connection is handled in a forked child: LX200 clients hold the socket
 * open and poll once a second, which would otherwise block the HTTP server for
 * the entire session.
 * =========================================================================== */

static void lx_send(int fd, const char *s2) { send_all(fd, s2, strlen(s2)); }

/* degrees -> "HH:MM:SS" (RA) */
static void fmt_ra(double deg, char *out, size_t cap) {
    double h = deg / 15.0;
    int hh, mm, ss;
    while (h < 0) h += 24.0;
    while (h >= 24.0) h -= 24.0;
    hh = (int)h;
    mm = (int)((h - hh) * 60.0);
    ss = (int)(((h - hh) * 60.0 - mm) * 60.0 + 0.5);
    if (ss >= 60) { ss -= 60; mm++; }
    if (mm >= 60) { mm -= 60; hh = (hh + 1) % 24; }
    snprintf(out, cap, "%02d:%02d:%02d", hh, mm, ss);
}

/* degrees -> "sDD*MM:SS" (Dec) */
static void fmt_dec(double deg, char *out, size_t cap) {
    char sign = deg < 0 ? '-' : '+';
    int dd, mm, ss;
    double a = deg < 0 ? -deg : deg;
    dd = (int)a;
    mm = (int)((a - dd) * 60.0);
    ss = (int)(((a - dd) * 60.0 - mm) * 60.0 + 0.5);
    if (ss >= 60) { ss -= 60; mm++; }
    if (mm >= 60) { mm -= 60; dd++; }
    snprintf(out, cap, "%c%02d*%02d:%02d", sign, dd, mm, ss);
}

/* "HH:MM:SS" or "HH:MM.T" -> degrees; -1 on parse failure */
static double parse_ra(const char *s2) {
    int hh = 0, mm = 0; double ss = 0;
    if (sscanf(s2, "%d:%d:%lf", &hh, &mm, &ss) >= 2)
        return (hh + mm / 60.0 + ss / 3600.0) * 15.0;
    return -1.0;
}

/* "sDD*MM:SS" / "sDD:MM:SS" -> degrees; -999 on failure */
static double parse_dec(const char *s2) {
    int sign = 1, dd = 0, mm = 0; double ss = 0;
    const char *p2 = s2;
    if (*p2 == '+') p2++;
    else if (*p2 == '-') { sign = -1; p2++; }
    if (sscanf(p2, "%d%*[*:\xdf]%d:%lf", &dd, &mm, &ss) >= 2)
        return sign * (dd + mm / 60.0 + ss / 3600.0);
    if (sscanf(p2, "%d%*[*:\xdf]%d", &dd, &mm) == 2)
        return sign * (dd + mm / 60.0);
    return -999.0;
}

static void lx200_session(int fd) {
    char buf[512];
    double tgt_ra = 0, tgt_dec = 0;
    int have_ra = 0, have_dec = 0;
    size_t used = 0;

    for (;;) {
        ssize_t n = read(fd, buf + used, sizeof buf - 1 - used);
        char *p2;
        if (n <= 0) return;
        used += (size_t)n;
        buf[used] = 0;

        /* commands are ":CMD#"; process each complete one */
        while ((p2 = memchr(buf, '#', used)) != NULL) {
            char cmd[128];
            size_t len = (size_t)(p2 - buf);
            if (len >= sizeof cmd) len = sizeof cmd - 1;
            memcpy(cmd, buf, len); cmd[len] = 0;
            /* consume through the '#' */
            memmove(buf, p2 + 1, used - (size_t)(p2 - buf) - 1);
            used -= (size_t)(p2 - buf) + 1;
            buf[used] = 0;

            {
                const char *c = cmd;
                char out[128];
                while (*c == ':' || *c == ' ') c++;

                if (!strncmp(c, "GR", 2)) {
                    double ra = 0, dec = 0;
                    current_radec(&ra, &dec);
                    fmt_ra(ra, out, sizeof out);
                    strcat(out, "#"); lx_send(fd, out);
                } else if (!strncmp(c, "GD", 2)) {
                    double ra = 0, dec = 0;
                    current_radec(&ra, &dec);
                    fmt_dec(dec, out, sizeof out);
                    strcat(out, "#"); lx_send(fd, out);
                } else if (!strncmp(c, "Sr", 2)) {
                    double v = parse_ra(c + 2);
                    if (v >= 0) { tgt_ra = v; have_ra = 1; lx_send(fd, "1"); }
                    else lx_send(fd, "0");
                } else if (!strncmp(c, "Sd", 2)) {
                    double v = parse_dec(c + 2);
                    if (v > -999.0) { tgt_dec = v; have_dec = 1; lx_send(fd, "1"); }
                    else lx_send(fd, "0");
                } else if (!strncmp(c, "MS", 2)) {
                    if (!have_ra || !have_dec) {
                        lx_send(fd, "1target not set#");
                    } else {
                        char cm[512], o2[512];
                        snprintf(cm, sizeof cm,
                          "%s/polaris-mount --host %s --port %d --lat %.6f --lon %.6f goto-radec --refine-arcmin 0.1 --ra %.6f --dec %.6f 2>&1",
                          g_astro, g_mount_host, g_mount_port, g_lat, g_lon, tgt_ra, tgt_dec);
                        run_capture(cm, o2, sizeof o2);
                        lx_send(fd, "0");          /* 0 = slew started */
                    }
                } else if (!strncmp(c, "CM", 2)) {
                    if (have_ra && have_dec) {
                        char cm[512], o2[512];
                        snprintf(cm, sizeof cm,
                          "%s/polaris-mount --host %s --port %d --lat %.6f --lon %.6f align --solved-ra %.6f --solved-dec %.6f 2>&1",
                          g_astro, g_mount_host, g_mount_port, g_lat, g_lon, tgt_ra, tgt_dec);
                        run_capture(cm, o2, sizeof o2);
                    }
                    lx_send(fd, "Aligned#");
                } else if (!strncmp(c, "Q", 1)) {
                    char cm[256], o2[256];
                    snprintf(cm, sizeof cm, "%s/polaris-mount --host %s --port %d abort 2>&1", g_astro, g_mount_host, g_mount_port);
                    run_capture(cm, o2, sizeof o2);
                    /* :Q# has no reply */
                } else if (!strncmp(c, "GVP", 3)) { lx_send(fd, "Benro Polaris#");
                } else if (!strncmp(c, "GVN", 3)) { lx_send(fd, "1.0#");
                } else if (!strncmp(c, "GVD", 3)) { lx_send(fd, "polaris-httpd#");
                } else if (!strncmp(c, "GVT", 3)) { lx_send(fd, "00:00:00#");
                } else if (!strncmp(c, "Gt", 2)) {
                    fmt_dec(g_lat, out, sizeof out); strcat(out, "#"); lx_send(fd, out);
                } else if (!strncmp(c, "Gg", 2)) {
                    /* LX200 reports longitude POSITIVE WEST -- the opposite of
                     * the usual convention. Getting this backwards puts a
                     * client's horizon on the wrong side of the sky. */
                    fmt_dec(-g_lon, out, sizeof out); strcat(out, "#"); lx_send(fd, out);
                } else if (!strncmp(c, "U", 1)) {
                    /* precision toggle: accepted, we always send high precision */
                } else {
                    /* Unknown commands get no reply, which is what a real LX200
                     * does; clients probe with commands the mount may not have. */
                }
            }
        }
        if (used >= sizeof buf - 1) used = 0;      /* garbage; resync */
    }
}

/* --------------------------------------------------------------- web page */
static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1,viewport-fit=cover'>"
"<title>Polaris Plate Solver</title>"
"<link rel=icon href='/icon.svg' type='image/svg+xml'>"
"<link rel='apple-touch-icon' href='/icon.svg'>"
"<meta name='apple-mobile-web-app-capable' content='yes'>"
"<meta name='apple-mobile-web-app-status-bar-style' content='black-translucent'>"
"<meta name='apple-mobile-web-app-title' content='Polaris'>"
"<meta name='theme-color' content='#0b0e14'>"
"<style>"
":root{--bg:#0b0e14;--fg:#e6e6e6;--dim:#8b93a7;--acc:#5aa9e6;--ok:#4caf50;--err:#e05252;--card:#151a23}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);"
"font:15px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
"padding:16px;padding:max(16px,env(safe-area-inset-top)) 16px 24px}"
"h1{font-size:19px;margin:0 0 14px;display:flex;align-items:center;gap:10px}"
"h1 img{width:26px;height:26px;border-radius:6px}"
"h2{font-size:13px;text-transform:uppercase;"
"letter-spacing:.08em;color:var(--dim);margin:0 0 8px}"
".card{background:var(--card);border-radius:12px;padding:14px;margin-bottom:12px;"
"border:1px solid #1e2532}"
/* WIDE SCREENS: a single 640px column wastes a desktop. Masonry-ish columns
 * keep the cards readable without stretching tables and charts to absurd
 * widths, and it degrades to one column on a phone with no media-query
 * gymnastics. */
"@media(min-width:900px){body{max-width:1400px;margin:0 auto}"
".wrap{column-count:2;column-gap:14px}"
".card{break-inside:avoid;-webkit-column-break-inside:avoid;display:inline-block;width:100%}}"
"@media(min-width:1500px){.wrap{column-count:3}}"
".row{display:flex;justify-content:space-between;gap:12px;padding:3px 0}"
".row span:last-child{font-variant-numeric:tabular-nums;text-align:right}"
"button{background:var(--acc);color:#04121f;border:0;border-radius:8px;padding:11px 16px;"
"font-size:15px;font-weight:600;cursor:pointer;margin:4px 6px 4px 0}"
"button.alt{background:#2c3444;color:var(--fg)}button:disabled{opacity:.45;cursor:default}"
"pre{background:#0a0d13;border-radius:8px;padding:10px;overflow:auto;max-height:260px;"
"font-size:12px;margin:0;white-space:pre-wrap;word-break:break-word}"
".s{display:inline-block;padding:2px 9px;border-radius:20px;font-size:12px;font-weight:600}"
".s.running{background:#3a3410;color:#e8c547}.s.done{background:#12331a;color:var(--ok)}"
".s.failed{background:#3a1414;color:var(--err)}.s.idle{background:#232a36;color:var(--dim)}"
"</style></head><body>"
"<h1><img src='/icon.svg' alt=''>Benro Polaris &mdash; Plate Solver</h1>"
"<div class=wrap>"
"<div class=card><h2>Solve</h2>"
"<button id=b0 onclick=go(1,'capture')>Capture, Solve &amp; Apply</button>"
"<button id=b1 class=alt onclick=go(0,'latest')>Solve Latest Frame</button>"
"<button id=b2 class=alt onclick=go(0,'wait')>Wait for Next Shot</button>"
"<button id=b3 class=alt onclick=go(1,'latest')>Solve Latest &amp; Apply</button>"
"<button id=b5 class=alt onclick=applylast()>Apply Last Solution</button>"
"<button id=b4 class=alt onclick=cancel()>Cancel</button>"
"<div id=msg style='color:var(--dim);font-size:12.5px;margin-top:8px'></div>"
"<div style='color:var(--dim);font-size:12.5px;margin-top:8px'>"
"<b>Capture, Solve &amp; Apply</b> fires one frame (opcode 264), solves it and pushes the "
"correction &mdash; the whole loop in one press. It works once compass alignment is done, but "
"<b>not while astro mode owns the shutter</b>: there the device ignores an external capture, so "
"take the shot in the Benro app and use <b>Solve Latest</b>. <b>Apply Last Solution</b> pushes "
"the solve already on screen without redoing it &mdash; it applies at that frame\u2019s time and "
"refuses if the frame is stale, since the mount has probably moved since.</div>"
"<div class=row style='margin-top:10px'><span>status</span>"
"<span><span id=st class='s idle'>idle</span></span></div></div>"
"<div class=card><h2>Last solution</h2><div id=sol></div></div>"
"<div class=card><h2>Focal length</h2>"
"<div id=fcs style='margin-bottom:8px'></div>"
"<input id=fin type=number min=1 max=100000 step=any placeholder='e.g. 400' "
  "style='background:#0a0d13;color:var(--fg);border:1px solid #2c3444;border-radius:8px;"
  "padding:10px;font-size:15px;width:130px;margin-right:6px'>"
"<button style='padding:9px 14px;font-size:14px' onclick=setf()>Set</button>"
"<button class=alt style='padding:9px 14px;font-size:14px' onclick=setf('auto')>Auto</button>"
"<div id=ferr style='color:var(--err);font-size:12.5px;margin-top:6px'></div>"
"<div style='color:var(--dim);font-size:12.5px;margin-top:6px'>Auto reads the focal from each "
"frame&rsquo;s EXIF. Set it manually for a telescope or a manual lens, which report none &mdash; "
"a wrong focal makes solving impossible, not just slow.</div></div>"
"<div class=card><h2>Mount</h2><div id=mnt></div>"
"<button class=alt style='margin-top:8px;padding:7px 12px;font-size:13px' onclick=mnow()>Read Mount</button>"
"<div style='color:var(--dim);font-size:12.5px;margin-top:6px'>Read on demand only &mdash; polling the mount "
"makes the Benro app re-prompt for compass calibration.</div></div>"
"<div class=card><h2>Home network</h2><div id=nws style='margin-bottom:8px'></div>"
"<input id=nssid placeholder='Wi-Fi name (SSID)' autocapitalize=off autocorrect=off "
  "style='background:#0a0d13;color:var(--fg);border:1px solid #2c3444;border-radius:8px;"
  "padding:10px;font-size:15px;width:100%;margin-bottom:6px'>"
"<input id=npsk type=password placeholder='Password' autocomplete='new-password' "
  "style='background:#0a0d13;color:var(--fg);border:1px solid #2c3444;border-radius:8px;"
  "padding:10px;font-size:15px;width:100%;margin-bottom:6px'>"
"<button style='padding:9px 14px;font-size:14px' onclick=wsave()>Save</button>"
"<button class=alt style='padding:9px 14px;font-size:14px' onclick=wact('up')>Join</button>"
"<button class=alt style='padding:9px 14px;font-size:14px' onclick=wact('down')>Disconnect</button>"
"<label style='display:block;margin-top:8px;font-size:13.5px;cursor:pointer'>"
"<input type=checkbox id=naj onchange=wauto() style='vertical-align:-2px;margin-right:6px'>"
"Join automatically at power-up</label>"
"<div id=nerr style='color:var(--err);font-size:12.5px;margin-top:6px'></div>"
"<pre id=nlog style='max-height:120px;margin-top:8px;display:none'></pre>"
"<div style='color:var(--dim);font-size:12.5px;margin-top:6px'>Joins your home network on a "
"<b>second</b> interface &mdash; the Polaris access point keeps running, so the Benro app still "
"reaches it at 192.168.0.1. Only the hashed key is stored, never the password. WPA2 only: "
"a WPA3-only network will not associate. If joining fails, a watchdog removes the interface "
"and leaves the access point untouched.</div></div>"
"<div class=card><h2>Wi-Fi sleep</h2><div id=kws style='margin-bottom:8px'></div>"
"<button style='padding:9px 14px;font-size:14px' onclick=kw(1)>Keep Awake</button>"
"<button class=alt style='padding:9px 14px;font-size:14px' onclick=kw('force')>Force Awake</button>"
"<button class=alt style='padding:9px 14px;font-size:14px' onclick=kw(0)>Allow Sleep</button>"
"<div style='color:var(--dim);font-size:12.5px;margin-top:6px'>The Polaris powers its "
"Wi-Fi down <b>60 s after the Benro app disconnects</b>, taking this page, ssh, Alpaca "
"and LX200 with it &mdash; firmware behaviour, with no setting. Keep Awake holds one "
"connection that registers itself as a client the way the app does, so the timer never "
"starts. It uses battery, and it waits until the mount is aligned before connecting, "
"because connecting earlier makes the Benro app ask for a compass calibration. "
"<b>Force Awake</b> skips that wait \u2014 useful for working on the box without keeping a "
"phone in the app, at the cost of possibly triggering that dialog.</div></div>"
"<div class=card><h2>Guiding</h2>"
"<div id=gst style='margin-bottom:8px'></div>"
"<svg id=gchart viewBox='0 0 640 180' preserveAspectRatio='none' "
  "style='width:100%;height:180px;background:#0a0d13;border-radius:8px;display:block'></svg>"
"<div id=glegend style='color:var(--dim);font-size:12px;margin-top:6px'></div>"
"<button id=g1 style='padding:9px 14px;font-size:14px;margin-top:8px' onclick=guide(1)>Start Guiding</button>"
"<button id=g0 class=alt style='padding:9px 14px;font-size:14px;margin-top:8px' onclick=guide(0)>Stop</button>"
"<div id=gerr style='color:var(--err);font-size:12.5px;margin-top:6px'></div>"
"<div style='color:var(--dim);font-size:12.5px;margin-top:6px'>Guiding measures drift against an "
"anchor frame and re-centres when it exceeds the threshold. It needs a solved position to anchor "
"on, and an aligned mount &mdash; it will refuse without both. Corrections are small slews, not "
"rate adjustments: the mount exposes no axis-rate primitive.</div></div>"
"<div class=card><h2>Log</h2><pre id=log>&hellip;</pre></div>"
"</div>"
"<script>\n"
"function f(n,d){return (n===undefined||n===null||isNaN(n))?'--':Number(n).toFixed(d)}\n"
"function hms(deg){if(deg==null||isNaN(deg))return '--';var h=deg/15,H=Math.floor(h),"
"m=(h-H)*60,M=Math.floor(m),S=(m-M)*60;return H+'h '+M+'m '+S.toFixed(1)+'s'}\n"
"function dms(deg){if(deg==null||isNaN(deg))return '--';var s=deg<0?'-':'+';deg=Math.abs(deg);"
"var D=Math.floor(deg),m=(deg-D)*60,M=Math.floor(m),S=(m-M)*60;"
"return s+D+'\\u00b0 '+M+'\\u2032 '+S.toFixed(1)+'\\u2033'}\n"
"function row(k,v){return '<div class=row><span>'+k+'</span><span>'+v+'</span></div>'}\n"
"var BTNS=['b0','b1','b2','b3','b5'];\n"
"function go(a,m){BTNS.forEach(i=>document.getElementById(i).disabled=true);\n"
"  fetch('/api/solve?mode='+m+(a?'&apply=1':''),{method:'POST'})\n"
"   .then(r=>r.json()).then(d=>{\n"
"     if(d.started===false)document.getElementById('msg').textContent=\n"
"       (d.reason||'did not start')+' ('+(d.elapsed_sec||0)+'s so far) \u2014 press Cancel to stop it';\n"
"     else document.getElementById('msg').textContent='';\n"
"     tick()})}\n"
"function applylast(){var m=document.getElementById('msg');m.textContent='applying\u2026';\n"
"  fetch('/api/apply',{method:'POST'}).then(r=>r.json()).then(function(d){\n"
"    m.textContent=d.ok?('applied the last solution ('+(d.age_sec||0)+'s old)'):(d.error||'failed');\n"
"    tick()})}\n"
"function cancel(){fetch('/api/cancel',{method:'POST'}).then(r=>r.json()).then(d=>{\n"
"  document.getElementById('msg').textContent=d.cancelled?'cancelled':(d.reason||'nothing to cancel');\n"
"  tick()})}\n"
"function tick(){fetch('/api/state').then(r=>r.json()).then(s=>{\n"
"  var st=document.getElementById('st');\n"
"  st.textContent=s.status+((s.status==='running'&&s.elapsed_sec)?' '+s.elapsed_sec+'s':'');\n"
"  st.className='s '+s.status;\n"
"  if(s.status==='running'&&s.elapsed_sec>20)document.getElementById('msg').textContent=\n"
"    'searching a range of focal lengths \u2014 this takes up to ~4 min; set a focal length below to make it fast';\n"
"  var busy=s.status==='running';rate(busy);\n"
"  BTNS.forEach(i=>document.getElementById(i).disabled=busy);\n"
"  document.getElementById('b4').disabled=!busy;\n"
"  var o=s.solution;\n"
"  document.getElementById('sol').innerHTML = (o&&o.solved)\n"
"    ? row('RA',hms(o.ra_deg))+row('Dec',dms(o.dec_deg))+row('roll',f(o.roll_deg,2)+'\\u00b0')\n"
"      +row('pixel scale',f(o.pixscale_arcsec,3)+'\\u2033/px')\n"
"      +row('field',f(o.field_w_deg,2)+'\\u00b0 \\u00d7 '+f(o.field_h_deg,2)+'\\u00b0')\n"
"      +row('matches',o.nmatch+' of '+o.nfield)+row('solve time',f(o.solve_seconds,2)+' s')\n"
"    : '<div class=row><span>no solution yet</span><span></span></div>';\n"
"  document.getElementById('mnt').innerHTML=row('aligned',s.aligned?'yes':'no')\n"
"    +row('tracking',s.tracking?'yes':'no')+(s.mount_read?'':'');\n"
"  if(s.mount_read){document.getElementById('mnt').innerHTML=\n"
"    row('alt',f(s.alt,3)+'\\u00b0')+row('az',f(s.az,3)+'\\u00b0')\n"
"    +row('aligned',s.aligned?'yes':'no')+row('tracking',s.tracking?'yes':'no')}\n"
"  document.getElementById('log').textContent=s.log||'(empty)';\n"
"})}\n"
"function fshow(d){var t;\n"
"  if(d.override) t='<b>'+d.override+' mm</b> &mdash; manual override';\n"
"  else if(d.exif_cache) t='auto &mdash; last frame reported <b>'+d.exif_cache+' mm</b>';\n"
"  else t='auto &mdash; no focal seen yet; will search a range';\n"
"  document.getElementById('fcs').innerHTML=t}\n"
"function fload(){fetch('/api/focal').then(r=>r.json()).then(fshow)}\n"
"function setf(v){var e=document.getElementById('ferr');e.textContent='';\n"
"  var val=(v==='auto')?'auto':document.getElementById('fin').value;\n"
"  if(v!=='auto'&&!val){e.textContent='enter a focal length in mm';return}\n"
"  fetch('/api/focal?focal='+encodeURIComponent(val),{method:'POST'})\n"
"   .then(r=>r.json()).then(d=>{if(d.ok){if(v==='auto')document.getElementById('fin').value='';fshow(d)}\n"
"     else e.textContent=d.error||'failed'})}\n"

"function kwshow(d){var t;\n"
"  if(d.enabled&&d.running&&d.forced)t='<b>keeping Wi-Fi awake</b> \u2014 forced, ignoring alignment';\n"
"  else if(d.enabled&&d.running)t=d.aligned?'<b>keeping Wi-Fi awake</b>':'<b>armed</b> \u2014 waiting for the mount to be aligned';\n"
"  else if(d.enabled)t='<b>enabled</b> but the helper is not running';\n"
"  else t='Wi-Fi will sleep 60 s after the app disconnects (firmware default)';\n"
"  document.getElementById('kws').innerHTML=t}\n"
"function kwload(){fetch('/api/keepwifi').then(r=>r.json()).then(kwshow)}\n"
"function kw(v){fetch('/api/keepwifi?on='+v,{method:'POST'}).then(r=>r.json()).then(kwshow)}\n"
"function gchart(d){var s=document.getElementById('gchart'),p=d.points||[],W=640,H=180,pad=24;\n"
"  if(!p.length){s.innerHTML='<text x=\"320\" y=\"95\" fill=\"#8b93a7\" font-size=\"13\" "
"text-anchor=\"middle\">no drift measurements yet</text>';return}\n"
"  var th=parseFloat(d.threshold)||60;\n"
"  var mx=Math.max(th*1.3,Math.max.apply(null,p.map(function(q){return q.d})) *1.15);\n"
"  var n=p.length,dx=(W-pad*2)/Math.max(n-1,1);\n"
"  function X(i){return pad+i*dx}\n"
"  function Y(v){return H-pad-(v/mx)*(H-pad*2)}\n"
"  var o='';\n"
"  o+='<line x1=\"'+pad+'\" y1=\"'+Y(th)+'\" x2=\"'+(W-pad)+'\" y2=\"'+Y(th)+'\" "
"stroke=\"#e05252\" stroke-width=\"1\" stroke-dasharray=\"4 3\"/>';\n"
"  o+='<text x=\"'+(W-pad)+'\" y=\"'+(Y(th)-4)+'\" fill=\"#e05252\" font-size=\"11\" "
"text-anchor=\"end\">threshold '+th+'\u2033</text>';\n"
"  o+='<line x1=\"'+pad+'\" y1=\"'+(H-pad)+'\" x2=\"'+(W-pad)+'\" y2=\"'+(H-pad)+'\" stroke=\"#2c3444\"/>';\n"
"  var dpath='';\n"
"  for(var i=0;i<n;i++){dpath+=(i?' L':'M')+X(i).toFixed(1)+' '+Y(p[i].d).toFixed(1)}\n"
"  o+='<path d=\"'+dpath+'\" fill=\"none\" stroke=\"#5aa9e6\" stroke-width=\"1.6\"/>';\n"
"  for(var i=0;i<n;i++){var c=p[i].c;\n"
"    o+='<circle cx=\"'+X(i).toFixed(1)+'\" cy=\"'+Y(p[i].d).toFixed(1)+'\" r=\"'+(c?3.2:2)+'\" fill=\"'+(c?'#e8c547':'#5aa9e6')+'\"><title>'+p[i].t+'  '+p[i].d+'\u2033'+(c?' (corrected)':'')+'</title></circle>'}\n"
"  o+='<text x=\"'+pad+'\" y=\"14\" fill=\"#8b93a7\" font-size=\"11\">arcsec</text>';\n"
"  o+='<text x=\"'+pad+'\" y=\"'+(H-6)+'\" fill=\"#8b93a7\" font-size=\"11\">'+p[0].t+'</text>';\n"
"  o+='<text x=\"'+(W-pad)+'\" y=\"'+(H-6)+'\" fill=\"#8b93a7\" font-size=\"11\" text-anchor=\"end\">'+p[n-1].t+'</text>';\n"
"  s.innerHTML=o;\n"
"  var corr=p.filter(function(q){return q.c}).length;\n"
"  document.getElementById('glegend').innerHTML='last '+n+' checks \u00b7 '+corr+' correction'+(corr==1?'':'s')+' \u00b7 peak '+Math.max.apply(null,p.map(function(q){return q.d})).toFixed(1)+'\u2033'}\n"
"function gshow(d){document.getElementById('gst').innerHTML=d.running?'<b>guiding</b> \u00b7 every '+d.interval+'s, threshold '+d.threshold+'\u2033':'not guiding';\n"
"  document.getElementById('g1').disabled=!!d.running;document.getElementById('g0').disabled=!d.running;\n"
"  gchart(d)}\n"
"function gload(){fetch('/api/guide').then(r=>r.json()).then(gshow)}\n"
"function guide(v){document.getElementById('gerr').textContent='';\n"
"  fetch('/api/guide?on='+v,{method:'POST'}).then(r=>r.json()).then(function(d){\n"
"    if(d.ok===false)document.getElementById('gerr').textContent=d.error||'failed';\n"
"    else gshow(d)})}\n"
"gload();setInterval(gload,10000);\n"
"function wshow(d){var t='';\n"
"  t+='Access point: <b>'+(d.ap_ip||'down')+'</b>';\n"
"  t+=' &middot; home network: <b>'+(d.sta_ip||'not joined')+'</b>';\n"
"  if(d.configured)t+='<br>configured for <b>'+(d.ssid||'(unknown)')+'</b>';\n"
"  var cb=document.getElementById('naj'); if(cb)cb.checked=!!d.autojoin;\n"
"  else t+='<br>no network configured yet';\n"
"  document.getElementById('nws').innerHTML=t}\n"
"function wload(){fetch('/api/wifi').then(r=>r.json()).then(wshow)}\n"
"function wsave(){var e=document.getElementById('nerr');e.textContent='';\n"
"  var s=document.getElementById('nssid').value,p=document.getElementById('npsk').value;\n"
"  if(!s||p.length<8){e.textContent='enter a network name and a password of at least 8 characters';return}\n"
"  fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},\n"
"    body:'action=save&ssid='+encodeURIComponent(s)+'&psk='+encodeURIComponent(p)})\n"
"   .then(r=>r.json()).then(function(d){\n"
"     document.getElementById('npsk').value='';\n"
"     if(d.ok)e.style.color='var(--ok)',e.textContent='saved (only the hashed key is stored)';\n"
"     else e.style.color='var(--err)',e.textContent=d.error||'failed';\n"
"     wload()})}\n"
"function wauto(){var on=document.getElementById('naj').checked;\n"
"  fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},\n"
"    body:'action=autojoin_'+(on?'on':'off')}).then(function(){\n"
"      document.getElementById('nerr').style.color='var(--dim)';\n"
"      document.getElementById('nerr').textContent=on?'will join automatically at power-up':'auto-join off';\n"
"      wload()})}\n"
"function wact(a){var e=document.getElementById('nerr');e.style.color='var(--dim)';\n"
"  e.textContent=(a=='up'?'joining\u2026 (up to 45s)':'disconnecting\u2026');\n"
"  fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},\n"
"    body:'action='+a}).then(r=>r.json()).then(function(d){\n"
"      var L=document.getElementById('nlog');L.style.display='block';L.textContent=d.log||'';\n"
"      e.textContent='';wload()})}\n"
"wload();\n"
"kwload();\n"
"fload();\n"
"tick();var iv=setInterval(tick,4000);\n// poll fast only while a job is actually running\nfunction rate(busy){clearInterval(iv);iv=setInterval(tick,busy?1500:4000)}\n\n"
"</script></body></html>";

/* ------------------------------------------------------------------- main */

/* ===================== the control link and its policy =====================
 *
 * The web UI reaches the mount through ONE persistent connection (polaris-link)
 * rather than by running polaris-mount per request -- see polaris-link.h for
 * why. What follows is the gate in front of it.
 *
 * DEFAULT DENY. The UI can only send opcodes named here. That is not
 * ceremony: this protocol has commands that wedge the motors (530 -- field
 * traces from the Aperion work show repeated 530s jamming the head) and
 * commands that write persistent state, and a control surface reachable by
 * anything on the wifi should not be one typo in a fetch() away from either.
 * An opcode absent from this table is refused with its number in the error, so
 * extending the table is the obvious next step rather than a mystery.
 *
 * `moves` marks anything that can drive a motor. Those are refused unless the
 * request carries confirm=1, so a page reload or a stray retry cannot slew the
 * head while the user is looking through the camera.
 */
typedef struct {
    int         cmd;
    const char *name;
    int         sub;        /* subsystem selector -- see below             */
    int         moves;      /* 1 => needs confirm=1                        */
    const char *note;
} opcode_policy_t;

/* THE SUBSYSTEM SELECTOR IS PER-OPCODE AND THE TABLE OWNS IT.
 *
 * The third wire field is not a constant: camera parameters ride 1, status and
 * capture ride 2, gimbal motion rides 3, camera identity rides 4 -- and there
 * are exceptions inside those ranges (520 and 526 use 2 despite being 5xx).
 * A command sent on the wrong subsystem is not rejected, it is IGNORED, which
 * debugs like a dead mount. So the client does not get to choose: it names an
 * opcode and the server supplies the selector documented for it.
 *
 * Values below are from docs/APP-PROTOCOL.md §5, each traceable to the
 * decompiled app. An opcode whose payload is not documented there does not
 * belong in this table yet -- see the note at the end.
 */
static const opcode_policy_t OPCODES[] = {
    /* --- read-only telemetry and identity ------------------------------ */
    { 284, "mode",          3, 0, "operating mode + tracking flag" },
    { 517, "raw-angles",    3, 0, "raw motor angles (radians, pitch negated)" },
    { 518, "pose",          3, 0, "quaternion + compass + altitude" },
    { 778, "battery",       2, 0, "capacity + charging state" },
    { 775, "sd-info",       2, 0, "card status, total/free/used space" },
    { 286, "camera-info",   4, 0, "connected camera identity" },
    { 282, "image-format",  4, 0, "current image format" },
    { 292, "preview-state", 2, 0, "is the camera preview stream on" },

    /* --- camera option lists. Fetch these BEFORE setting anything: the
     *     set commands take an INDEX into these lists, not a physical value,
     *     so a hardcoded table silently sets the wrong exposure. ---------- */
    { 265, "iso-options",     1, 0, "ISO option list + selected index" },
    { 266, "wb-options",      1, 0, "white-balance option list + index" },
    { 267, "ev-options",      1, 0, "exposure-compensation list + index" },
    { 268, "shutter-options", 1, 0, "shutter-speed list + index" },
    { 275, "aperture-options",1, 0, "aperture list + index" },

    /* --- camera settings. Index-valued; see above. --------------------- */
    { 258, "set-iso",       1, 0, "iso:<index into 265's list>" },
    { 259, "set-wb",        1, 0, "wb:<index into 266's list>" },
    { 260, "set-ev",        1, 0, "ev:<index into 267's list>" },
    { 261, "set-shutter",   1, 0, "s:<index into 268's list>" },
    { 276, "set-aperture",  1, 0, "fNum:<index into 275's list>" },
    { 262, "set-focus",     1, 0, "mod:<mode>;f:<value>" },
    { 311, "focus-adjust",  1, 0, "mode:<m>;adj:<v> -- nudges focus" },
    { 291, "set-preview",   2, 0, "state:<0|1> -- the MJPEG stream on :8080" },

    /* --- capture. Not motion, but these DO commit frames to the card, so
     *     they are confirmed like motion is: a stray retry should not fire
     *     the shutter during a long exposure. --------------------------- */
    { 264, "photo",         2, 1, "state:<s>;bulb:<b>;c:<c> -- fires the shutter" },
    { 263, "record",        2, 1, "state:<0|1> -- video record start/stop" },

    /* --- motion ---------------------------------------------------------
     * The jog opcodes (513/514/521, 532/533/534) are deliberately NOT here:
     * they go through /api/move, which owns the 50 ms repeat and the dead-man
     * stop. Reaching them through the raw send endpoint would bypass both and
     * is exactly how an axis gets left running. */
    { 519, "goto",          3, 1, "state;yaw;pitch;lat;lng;track;speed -- slews" },
    { 531, "track",         3, 1, "sidereal tracking on/off" },
    { 523, "recentre",      3, 1, "axis:<1|2|3> -- re-zero one axis" },
    { 549, "auto-level",    3, 1, "state:<0|1> -- actively levels the head" },

    /* --- alignment ------------------------------------------------------ */
    { 527, "set-heading",   3, 1, "compass heading; UNSIGNED [0,360) offset by 180" },
    { 547, "auto-level-en", 3, 0, "read the auto-level enable flag" },
    { 548, "set-auto-level-en", 3, 0, "en:<0|1> -- persistent" },
    /* Tilt compensation: the head corrects for a non-level base from its own
     * attitude, so deep-sky tracking does not need a perfectly level tripod. A
     * persistent SETTING, not motion -- so it is not confirm-gated. (538) */
    { 537, "tilt-comp-get", 3, 0, "read the tilt-compensation flag -> state:<0|1>" },
    { 538, "tilt-comp",     3, 0, "state:<0|1> -- compensate for an unlevel base; persistent" },

    /* NOT LISTED, DELIBERATELY:
     *   530  multi-step star alignment -- wedges the motors on repeat, and the
     *        app, the alpaca driver and polaris-mount.c describe three
     *        DIFFERENT step sequences. One 527 does the job.
     *   542  releases the travel limits -- and the polarity is inverted from
     *        the obvious reading (state:1 = limits OFF). A UI one fetch() away
     *        from letting the head swing into its own accessory is not a UI.
     *   772  deletes files.
     *   808  registration -- owned by the link layer.
     *   anything that writes flash or firmware.
     * Adding an opcode here means finding its payload in docs/APP-PROTOCOL.md
     * first. A guess in this table is a command sent blind at real hardware. */
};
#define OPCODE_COUNT (sizeof OPCODES / sizeof OPCODES[0])

static const opcode_policy_t *opcode_policy(int cmd) {
    size_t i;
    for (i = 0; i < OPCODE_COUNT; i++)
        if (OPCODES[i].cmd == cmd) return &OPCODES[i];
    return NULL;
}

/* Should the link be up? It is the radio-keepalive as a side effect, and
 * registering while UNALIGNED is what makes the Benro app demand a compass
 * calibration (wifi-keepalive.sh documents this at length). So the same rule
 * applies here: connect once the mount is aligned, or when the user has
 * explicitly forced it. */
static int link_wanted(void) {
    /* POLARIS_LINK_FORCE=1 brings the link up regardless of what we know about
     * alignment. It exists for two real cases: bench work against polaris-sim,
     * where there is no device log to learn alignment from, and diagnosing a
     * device whose log has been truncated (the firmware rotates /app/Mlog.txt,
     * which is exactly how "aligned" becomes unknowable mid-session). It is an
     * env var rather than a UI switch because the cost -- registering while
     * unaligned makes the Benro app demand a compass calibration -- should be
     * paid deliberately by someone who read this comment. */
    const char *f = getenv("POLARIS_LINK_FORCE");
    if (f && *f && *f != '0') return 1;
    if (access(KEEPWIFI_FORCE, F_OK) == 0) return 1;
    return alignment_state() == 1;
}

/* Ask for the things the head does NOT push.
 *
 * Pose, mode and raw angles stream on their own once registered. Battery, card
 * space and camera identity are request/response -- the phone app polls them on
 * a 2 s timer. We poll slower: nothing here changes fast, the AP is shared with
 * the live-view stream, and a status chip that is five seconds stale is honest
 * while a flooded radio is not.
 *
 * Camera identity is asked for less often still: it only changes when a body is
 * plugged or unplugged, and it rides subsystem 4 rather than 2. */
static void telemetry_poll(void) {
    static double last_fast = 0, last_slow = 0;
    double now = plink_now_ms();
    if (plink_status() != PLINK_UP) { last_fast = last_slow = 0; return; }
    if (now - last_fast > 5000.0) {
        last_fast = now;
        plink_send(778, 2, "");     /* battery: capacity + charging      */
        plink_send(775, 2, "");     /* card: status + free/total/used    */
        plink_send(517, 3, "");     /* mechanical pose (radians) -- for
                                     * FREE PROGRAM keyframe capture     */
    }
    if (now - last_slow > 30000.0) {
        last_slow = now;
        plink_send(286, 4, "");     /* camera identity                   */
        plink_send(292, 2, "");     /* is the preview stream running     */
    }
}

/* Bring the link up or down to match policy. Called from the event loop, so it
 * must be cheap and must not block. */
static void link_reconcile(void) {
    static int last = -1;
    int want = link_wanted();
    if (want == last) return;
    last = want;
    if (want) plink_open(g_mount_host, g_mount_port);
    else      plink_close();
}

/* Serve an embedded asset. Returns 1 if the path matched. */
static int serve_asset(int fd, const char *path) {
    size_t i;
    const char *want = (!strcmp(path, "/")) ? "/index.html" : path;
    for (i = 0; i < WEBUI_ASSET_COUNT; i++) {
        const webui_asset_t *a = &WEBUI_ASSETS[i];
        if (strcmp(a->path, want)) continue;
        {
            char hdr[512];
            int hl = snprintf(hdr, sizeof hdr,
                "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                "%s"
                /* The UI is versioned with the binary, so a long cache would
                 * strand a browser on the previous build after an upgrade --
                 * exactly the bug that is impossible to diagnose remotely.
                 * no-cache still allows a 304 revalidation, which is cheap. */
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n\r\n",
                a->ctype, a->len,
                a->gzip ? "Content-Encoding: gzip\r\n" : "");
            send_all(fd, hdr, (size_t)hl);
            send_all(fd, (const char *)a->body, a->len);
        }
        return 1;
    }
    return 0;
}

static void handle(int fd) {
    char req[8192], *body = NULL;
    ssize_t n;
    char method[16] = {0}, target[1024] = {0}, path[1024], *qs;

    /* Read headers, then DRAIN Content-Length bytes of body.
     *
     * A single read() is not enough: a PUT's body often arrives in a second TCP
     * segment, and closing the socket with unread data in flight makes the
     * kernel send RST -- the client sees "connection reset by peer" even though
     * we answered. Alpaca PUTs carry their parameters in the body, so this also
     * fixes reading them at all. */
    {
        size_t have = 0;
        char *hdr_end = NULL;
        long clen = 0;
        while (have < sizeof req - 1) {
            n = read(fd, req + have, sizeof req - 1 - have);
            if (n <= 0) break;
            have += (size_t)n;
            req[have] = 0;
            hdr_end = strstr(req, "\r\n\r\n");
            if (hdr_end) break;
        }
        if (!have) return;
        req[have] = 0;
        if (!hdr_end) hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) {
            const char *cl = strcasestr(req, "content-length:");
            body = hdr_end + 4;
            if (cl) clen = atol(cl + 15);
            {
                size_t body_have = have - (size_t)(body - req);
                while ((long)body_have < clen && have < sizeof req - 1) {
                    n = read(fd, req + have, sizeof req - 1 - have);
                    if (n <= 0) break;
                    have += (size_t)n;
                    body_have += (size_t)n;
                    req[have] = 0;
                }
            }
        }
    }
    if (sscanf(req, "%15s %1023s", method, target) != 2) return;

    if (!strcmp(method, "OPTIONS")) { respond(fd, 200, "text/plain", "", 0); return; }

    snprintf(path, sizeof path, "%s", target);
    qs = strchr(path, '?');
    if (qs) { *qs = 0; qs++; } else qs = (char *)"";

    /* Home-screen icon. Served inline so the page stays self-contained -- there
     * is no filesystem to drop a .png into and no CDN to reach. iOS uses
     * apple-touch-icon; everything else takes the SVG favicon. */
    if (!strcmp(path, "/icon.svg")) {
        static const char ICON[] =
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 180 180'>"
            "<defs><radialGradient id='s' cx='50%' cy='40%'>"
            "<stop offset='0%' stop-color='#1b2436'/><stop offset='100%' stop-color='#0b0e14'/>"
            "</radialGradient></defs>"
            "<rect width='180' height='180' rx='38' fill='url(#s)'/>"
            "<circle cx='52' cy='48' r='2.6' fill='#e6e6e6'/>"
            "<circle cx='128' cy='40' r='2' fill='#cfd6e4'/>"
            "<circle cx='142' cy='96' r='2.4' fill='#e6e6e6'/>"
            "<circle cx='38' cy='118' r='1.8' fill='#cfd6e4'/>"
            "<circle cx='96' cy='138' r='2.2' fill='#e6e6e6'/>"
            "<circle cx='74' cy='86' r='3.4' fill='#fff'/>"
            "<path d='M52 48 L74 86 L142 96 L96 138 Z' fill='none' stroke='#5aa9e6' "
            "stroke-width='2.4' stroke-linejoin='round' opacity='.9'/>"
            "<circle cx='90' cy='90' r='58' fill='none' stroke='#5aa9e6' stroke-width='3' "
            "opacity='.35'/>"
            "<path d='M90 26 v12 M90 142 v12 M26 90 h12 M142 90 h12' stroke='#5aa9e6' "
            "stroke-width='3' stroke-linecap='round' opacity='.55'/>"
            "</svg>";
        respond(fd, 200, "image/svg+xml", ICON, sizeof ICON - 1);
        return;
    }

    /* The new control app. The plate-solve dashboard that used to live here is
     * still built and still served at /legacy: it is field-tested, it is what
     * the docs screenshot, and retiring it the same day its replacement lands
     * would leave no way back if the new one misbehaves in the dark at 2 a.m.
     * It becomes a panel inside the new UI, and /legacy goes when that panel
     * has been used in the field. */
    if (!strcmp(path, "/legacy") || !strcmp(path, "/legacy.html")) {
        respond(fd, 200, "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
        return;
    }
    if (serve_asset(fd, path)) return;

    /* ------------------------------------------------------ control link */

    /* Everything the link has heard, plus its own health. One request, so the
     * UI can render a whole frame without fanning out. */
    if (!strcmp(path, "/api/link")) {
        const plink_stats_t *st = plink_stats();
        char out[16384];
        int n = 0, first = 1, i;
        double now = plink_now_ms();
        const char *status =
            st->status == PLINK_UP ? "up" :
            st->status == PLINK_CONNECTING ? "connecting" : "down";
        char esc[256];
        json_escape(st->last_error, esc, sizeof esc);
        n += snprintf(out + n, sizeof out - n,
            "{\"status\":\"%s\",\"wanted\":%s,\"connects\":%lu,\"drops\":%lu,"
            "\"rx\":%lu,\"tx\":%lu,\"parse_errors\":%lu,\"evictions\":%lu,"
            "\"up_ms\":%.0f,\"last_rx_age_ms\":%.0f,\"error\":\"%s\",\"opcodes\":{",
            status, link_wanted() ? "true" : "false",
            st->connects, st->drops, st->rx_msgs, st->tx_msgs,
            st->parse_errors, st->slot_evictions,
            st->up_since_ms ? now - st->up_since_ms : 0.0,
            st->last_rx_ms ? now - st->last_rx_ms : -1.0,
            esc);
        /* Dump every opcode the link has a payload for. The UI decodes these
         * itself, so a newly understood opcode needs no C change -- which is
         * the whole point while the protocol is still being mapped. */
        for (i = 0; i < PLINK_SLOTS && n < (int)sizeof out - 512; i++) {
            const plink_slot_t *sl = plink_get_index(i);
            char aesc[PLINK_ARGS_MAX * 2];
            if (!sl) continue;
            json_escape(sl->args, aesc, sizeof aesc);
            n += snprintf(out + n, sizeof out - n,
                "%s\"%d\":{\"args\":\"%s\",\"age_ms\":%.0f,\"count\":%lu}",
                first ? "" : ",", sl->cmd, aesc, now - sl->when_ms, sl->count);
            first = 0;
        }
        n += snprintf(out + n, sizeof out - n, "}}");
        respond_json(fd, out);
        return;
    }

    /* Send one allowlisted opcode. POST cmd=519&args=...&confirm=1 */
    if (!strcmp(path, "/api/link/send")) {
        char cmds[16] = "", args[PLINK_ARGS_MAX] = "", conf[8] = "";
        const opcode_policy_t *pol;
        long cmd = 0;
        if (strcmp(method, "POST") && strcmp(method, "PUT")) {
            respond(fd, 405, "text/plain", "POST only\n", 10); return;
        }
        if (!param(qs, "cmd", cmds, sizeof cmds) && body) param(body, "cmd", cmds, sizeof cmds);
        if (!param(qs, "args", args, sizeof args) && body) param(body, "args", args, sizeof args);
        if (!param(qs, "confirm", conf, sizeof conf) && body) param(body, "confirm", conf, sizeof conf);
        /* parse_*_strict return 1 on SUCCESS (they read as predicates at the
         * call sites in handle_alpaca). Inverting that here cost a debugging
         * round: every request answered "cmd must be a number" for a cmd that
         * was a perfectly good number. */
        if (!parse_long_strict(cmds, &cmd)) { respond_400(fd, "cmd must be a number"); return; }

        pol = opcode_policy((int)cmd);
        if (!pol) {
            char msg[192];
            snprintf(msg, sizeof msg,
                "{\"ok\":false,\"error\":\"opcode %ld is not allowed\","
                "\"hint\":\"add it to OPCODES[] in polaris-httpd.c once "
                "docs/APP-PROTOCOL.md documents its payload\"}", cmd);
            respond(fd, 403, "application/json", msg, strlen(msg));
            return;
        }
        if (pol->moves && !(conf[0] == '1' || !strcmp(conf, "true"))) {
            char msg[192];
            snprintf(msg, sizeof msg,
                "{\"ok\":false,\"error\":\"%s moves the mount; resend with confirm=1\"}",
                pol->name);
            respond(fd, 409, "application/json", msg, strlen(msg));
            return;
        }
        if (plink_status() != PLINK_UP) {
            const char *msg = "{\"ok\":false,\"error\":\"control link is down\"}";
            respond(fd, 503, "application/json", msg, strlen(msg));
            return;
        }
        /* The TABLE's subsystem, never the client's: see the comment above
         * OPCODES[]. A wrong selector is ignored by the head, not rejected. */
        if (plink_send((int)cmd, pol->sub, args) != 0) {
            const char *msg = "{\"ok\":false,\"error\":\"send failed\"}";
            respond(fd, 502, "application/json", msg, strlen(msg));
            return;
        }
        {
            char msg[128];
            snprintf(msg, sizeof msg, "{\"ok\":true,\"cmd\":%ld,\"name\":\"%s\"}",
                     cmd, pol->name);
            respond_json(fd, msg);
        }
        return;
    }

    /* ------------------------------------------------------------- motion */

    /* Hold-to-move. The BROWSER DOES NOT DRIVE THE WIRE: it declares an intent
     * with a short lease and re-affirms while a control is held, and the server
     * owns both the 50 ms fast-jog repeat and the stop. See polaris-jog.h --
     * the head is not known to stop on its own when a client goes away, so the
     * dead man has to live on this side. */
    if (!strcmp(path, "/api/move")) {
        char ax[8] = "", sp[16] = "", gr[16] = "";
        long speed = 0, gear = 0;
        jog_axis_t axis;
        char out[512];
        int n;
        if (strcmp(method, "POST") && strcmp(method, "PUT")) {
            respond(fd, 405, "text/plain", "POST only\n", 10); return;
        }
        if (!param(qs, "axis", ax, sizeof ax) && body) param(body, "axis", ax, sizeof ax);
        if (!param(qs, "speed", sp, sizeof sp) && body) param(body, "speed", sp, sizeof sp);
        if (!param(qs, "gear", gr, sizeof gr) && body) param(body, "gear", gr, sizeof gr);
        if      (!strcmp(ax, "pan"))  axis = JOG_PAN;
        else if (!strcmp(ax, "tilt")) axis = JOG_TILT;
        else if (!strcmp(ax, "rot"))  axis = JOG_ROT;
        else { respond_400(fd, "axis must be pan, tilt or rot"); return; }
        if (sp[0] && !parse_long_strict(sp, &speed)) { respond_400(fd, "speed must be a number"); return; }
        if (gr[0] && !parse_long_strict(gr, &gear))  { respond_400(fd, "gear must be a number"); return; }
        if (plink_status() != PLINK_UP) {
            const char *msg = "{\"ok\":false,\"error\":\"control link is down\"}";
            respond(fd, 503, "application/json", msg, strlen(msg));
            return;
        }
        if (jog_set(axis, (int)speed, (int)gear) != 0) {
            const char *msg = "{\"ok\":false,\"error\":\"jog refused\"}";
            respond(fd, 502, "application/json", msg, strlen(msg));
            return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"lease_ms\":%.0f,\"jog\":", JOG_LEASE_MS);
        n += jog_status_json(out + n, (int)sizeof out - n);
        snprintf(out + n, sizeof out - n, "}");
        respond_json(fd, out);
        return;
    }

    /* Stop everything. Deliberately answers 200 even with the link down: the
     * caller asked for motion to end, and "there is no link" means it already
     * has. A UI that treats stop as failable will show a scary error at the
     * exact moment the user is trying to make something stop. */
    if (!strcmp(path, "/api/stop")) {
        char out[512];
        int n;
        jog_stop_all();
        n = snprintf(out, sizeof out, "{\"ok\":true,\"jog\":");
        n += jog_status_json(out + n, (int)sizeof out - n);
        snprintf(out + n, sizeof out - n, "}");
        respond_json(fd, out);
        return;
    }

    /* ---------------------------------------------------------- programmes */

    /* Timelapse and panorama run ON THE HEAD once started; the server owns the
     * progress poll so a run stays observable with no client connected (see
     * polaris-prog.h). Both MOVE MOTORS, so both are gated on confirm=1. */
    if (!strcmp(path, "/api/prog/lapse")) {
        char iv[24]="", sh[24]="", bl[24]="", fp[24]="", conf[8]="";
        lapse_params_t lp; char err[160]="", out[512]; int n;
        if (strcmp(method, "POST") && strcmp(method, "PUT")) { respond(fd, 405, "text/plain", "POST only\n", 10); return; }
        if (!param(qs,"interval",iv,sizeof iv) && body) param(body,"interval",iv,sizeof iv);
        if (!param(qs,"shots",sh,sizeof sh) && body) param(body,"shots",sh,sizeof sh);
        if (!param(qs,"bulb",bl,sizeof bl) && body) param(body,"bulb",bl,sizeof bl);
        if (!param(qs,"fps",fp,sizeof fp) && body) param(body,"fps",fp,sizeof fp);
        if (!param(qs,"confirm",conf,sizeof conf) && body) param(body,"confirm",conf,sizeof conf);
        lp.interval_s = atof(iv);
        lp.shots = sh[0] ? atol(sh) : -1;
        lp.bulb_s = atof(bl);
        lp.fps = fp[0] ? atoi(fp) : 24;
        if (!(conf[0]=='1' || !strcmp(conf,"true"))) {
            /* No preview step for timelapse -- the guard is the confirm and the
             * derived-duration readout the UI shows before you press it. */
            respond(fd, 409, "application/json",
                "{\"ok\":false,\"error\":\"timelapse moves the mount and fires the shutter; resend with confirm=1\"}", 92);
            return;
        }
        if (prog_start_lapse(&lp, err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m));
            return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"prog\":");
        n += prog_status_json(out+n, (int)sizeof out - n);
        snprintf(out+n, sizeof out - n, "}");
        respond_json(fd, out);
        return;
    }

    /* Panorama. Because the START payload is inference (see polaris-prog.h), a
     * GET here (or POST without confirm) returns the EXACT frame that would be
     * sent, so a human can read it before committing. confirm=1 sends it. */
    if (!strcmp(path, "/api/prog/pano")) {
        char cols[16]="", rows[16]="", ha[24]="", va[24]="", sd[8]="", ps[8]="", iv[8]="", bl[24]="", isp[8]="", conf[8]="";
        pano_params_t pp; char err[160]="", out[1024]; int n;
        if (!param(qs,"cols",cols,sizeof cols) && body) param(body,"cols",cols,sizeof cols);
        if (!param(qs,"rows",rows,sizeof rows) && body) param(body,"rows",rows,sizeof rows);
        if (!param(qs,"hangle",ha,sizeof ha) && body) param(body,"hangle",ha,sizeof ha);
        if (!param(qs,"vangle",va,sizeof va) && body) param(body,"vangle",va,sizeof va);
        if (!param(qs,"startdir",sd,sizeof sd) && body) param(body,"startdir",sd,sizeof sd);
        if (!param(qs,"perspot",ps,sizeof ps) && body) param(body,"perspot",ps,sizeof ps);
        if (!param(qs,"interval",iv,sizeof iv) && body) param(body,"interval",iv,sizeof iv);
        if (!param(qs,"bulb",bl,sizeof bl) && body) param(body,"bulb",bl,sizeof bl);
        if (!param(qs,"isp",isp,sizeof isp) && body) param(body,"isp",isp,sizeof isp);
        if (!param(qs,"confirm",conf,sizeof conf) && body) param(body,"confirm",conf,sizeof conf);
        pp.h_num = cols[0] ? atoi(cols) : 0;
        pp.v_num = rows[0] ? atoi(rows) : 0;
        pp.h_angle = atof(ha);
        pp.v_angle = atof(va);
        pp.start_dir = sd[0] ? atoi(sd) : 1;
        pp.per_spot = ps[0] ? atoi(ps) : 1;
        pp.interval_s = iv[0] ? atoi(iv) : 0;
        pp.bulb_s = atof(bl);
        pp.isp = (isp[0]=='1' || !strcmp(isp,"true")) ? 1 : 0;

        if (!(conf[0]=='1' || !strcmp(conf,"true"))) {
            char frame[PLINK_ARGS_MAX], fesc[PLINK_ARGS_MAX*2], m[900];
            if (prog_check_pano(&pp, err, sizeof err) != 0) {
                json_escape(err, out, sizeof out);
                snprintf(m, sizeof m, "{\"ok\":false,\"valid\":false,\"error\":\"%s\"}", out);
                respond(fd, 200, "application/json", m, strlen(m));
                return;
            }
            prog_pano_preview(&pp, frame, sizeof frame);
            json_escape(frame, fesc, sizeof fesc);
            /* `inferred` is the whole reason this endpoint exists: the UI shows
             * the frame and says the binding is a guess until hardware confirms. */
            snprintf(m, sizeof m,
                "{\"ok\":true,\"valid\":true,\"preview\":true,\"inferred\":true,"
                "\"shots\":%ld,\"frame\":\"%s\"}",
                (long)pp.h_num*pp.v_num*pp.per_spot, fesc);
            respond(fd, 200, "application/json", m, strlen(m));
            return;
        }
        if (prog_start_pano(&pp, err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m));
            return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"prog\":");
        n += prog_status_json(out+n, (int)sizeof out - n);
        snprintf(out+n, sizeof out - n, "}");
        respond_json(fd, out);
        return;
    }

    /* HDR. GET/POST-without-confirm returns the exact frame AND the three real
     * shutter speeds it resolved to (from the camera's own list), so the bracket
     * is never a mystery before it fires. confirm=1 sends it. */
    if (!strcmp(path, "/api/prog/hdr")) {
        char sp[8]="", isp[8]="", conf[8]="";
        hdr_params_t hp; char err[160]="", out[1024]; int n;
        if (!param(qs,"spread",sp,sizeof sp) && body) param(body,"spread",sp,sizeof sp);
        if (!param(qs,"isp",isp,sizeof isp) && body) param(body,"isp",isp,sizeof isp);
        if (!param(qs,"confirm",conf,sizeof conf) && body) param(body,"confirm",conf,sizeof conf);
        hp.spread = sp[0] ? atoi(sp) : 3;
        hp.isp = (isp[0]=='1' || !strcmp(isp,"true")) ? 1 : 0;
        if (!(conf[0]=='1' || !strcmp(conf,"true"))) {
            char frame[600], fesc[1200], m[1400];
            if (prog_check_hdr(&hp, err, sizeof err) != 0) {
                json_escape(err, out, sizeof out);
                snprintf(m, sizeof m, "{\"ok\":false,\"valid\":false,\"error\":\"%s\"}", out);
                respond(fd, 200, "application/json", m, strlen(m)); return;
            }
            prog_hdr_preview(&hp, frame, sizeof frame);
            json_escape(frame, fesc, sizeof fesc);
            snprintf(m, sizeof m, "{\"ok\":true,\"valid\":true,\"preview\":true,\"frame\":\"%s\"}", fesc);
            respond(fd, 200, "application/json", m, strlen(m)); return;
        }
        if (prog_start_hdr(&hp, err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m)); return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"prog\":");
        n += prog_status_json(out+n, (int)sizeof out - n);
        snprintf(out+n, sizeof out - n, "}");
        respond_json(fd, out); return;
    }

    /* Focus stack. Setup (mark near/far, preview) then start. Racking the lens
     * between the marks goes through the ordinary 311 focus-adjust send. */
    if (!strcmp(path, "/api/prog/focus/mark")) {
        char v[8]="";
        if (!param(qs,"which",v,sizeof v) && body) param(body,"which",v,sizeof v);
        if (prog_focus_mark((v[0]=='1'||!strcmp(v,"far")||!strcmp(v,"end")) ? 1 : 0) != 0) {
            respond(fd, 503, "application/json", "{\"ok\":false,\"error\":\"control link is down\"}", 48); return;
        }
        respond_json(fd, "{\"ok\":true}"); return;
    }
    if (!strcmp(path, "/api/prog/focus")) {
        char sh[8]="", isp[8]="", prev[8]="", conf[8]="";
        focus_params_t fp; char err[160]="", out[512]; int n;
        if (!param(qs,"shots",sh,sizeof sh) && body) param(body,"shots",sh,sizeof sh);
        if (!param(qs,"isp",isp,sizeof isp) && body) param(body,"isp",isp,sizeof isp);
        if (!param(qs,"preview",prev,sizeof prev) && body) param(body,"preview",prev,sizeof prev);
        if (!param(qs,"confirm",conf,sizeof conf) && body) param(body,"confirm",conf,sizeof conf);
        fp.shots = sh[0] ? atoi(sh) : 14;
        fp.isp = (isp[0]=='1' || !strcmp(isp,"true")) ? 1 : 0;
        if (prev[0]=='1' || !strcmp(prev,"true")) {
            if (prog_focus_preview(&fp, err, sizeof err) != 0) {
                char m[256]; json_escape(err, out, sizeof out);
                snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
                respond(fd, 409, "application/json", m, strlen(m)); return;
            }
        } else if (conf[0]=='1' || !strcmp(conf,"true")) {
            if (prog_start_focus(&fp, err, sizeof err) != 0) {
                char m[256]; json_escape(err, out, sizeof out);
                snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
                respond(fd, 409, "application/json", m, strlen(m)); return;
            }
        } else {
            /* No confirm, no preview: just validate. */
            char m[256];
            if (prog_check_focus(&fp, err, sizeof err) != 0) {
                json_escape(err, out, sizeof out);
                snprintf(m, sizeof m, "{\"ok\":false,\"valid\":false,\"error\":\"%s\"}", out);
            } else snprintf(m, sizeof m, "{\"ok\":true,\"valid\":true}");
            respond(fd, 200, "application/json", m, strlen(m)); return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"prog\":");
        n += prog_status_json(out+n, (int)sizeof out - n);
        snprintf(out+n, sizeof out - n, "}");
        respond_json(fd, out); return;
    }

    /* Progress. Cheap read of server memory; the poll to the head runs in the
     * event loop, not here. */
    if (!strcmp(path, "/api/prog")) {
        char out[512];
        prog_status_json(out, sizeof out);
        respond_json(fd, out);
        return;
    }

    if (!strcmp(path, "/api/prog/cancel")) {
        prog_cancel();
        respond_json(fd, "{\"ok\":true}");
        return;
    }

    if (!strcmp(path, "/api/prog/pano/pause")) {
        char v[8]=""; int paused;
        if (!param(qs,"paused",v,sizeof v) && body) param(body,"paused",v,sizeof v);
        paused = (v[0]=='1' || !strcmp(v,"true"));
        if (prog_pano_pause(paused) != 0) { respond(fd, 409, "application/json", "{\"ok\":false,\"error\":\"no panorama running\"}", 46); return; }
        respond_json(fd, "{\"ok\":true}");
        return;
    }

    if (!strcmp(path, "/api/prog/pano/interval")) {
        char v[8]=""; long iv;
        if (!param(qs,"seconds",v,sizeof v) && body) param(body,"seconds",v,sizeof v);
        if (!parse_long_strict(v, &iv)) { respond_400(fd, "seconds must be a number"); return; }
        if (prog_pano_interval((int)iv) != 0) { respond(fd, 409, "application/json", "{\"ok\":false,\"error\":\"no panorama running\"}", 46); return; }
        respond_json(fd, "{\"ok\":true}");
        return;
    }

    /* -------------------------------------------------------------- astro */

    /* The astro session: mode 8 + the AHRS heartbeat, held open server-side so
     * neither lapses when the browser is away. Compass alignment is the browser
     * stand-in for the phone magnetometer; the solver on /legacy is the precise
     * path. See polaris-astro.h. */
    if (!strcmp(path, "/api/astro/enter")) {
        char err[160]="", out[256];
        if (astro_enter(g_lat, g_lon, err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 503, "application/json", m, strlen(m)); return;
        }
        respond_json(fd, "{\"ok\":true}"); return;
    }
    if (!strcmp(path, "/api/astro/leave")) {
        astro_leave();
        respond_json(fd, "{\"ok\":true}"); return;
    }
    if (!strcmp(path, "/api/astro/align")) {
        char v[24]="", err[160]="", out[256];
        if (!param(qs,"compass",v,sizeof v) && body) param(body,"compass",v,sizeof v);
        if (!v[0]) { respond_400(fd, "compass (degrees) required"); return; }
        if (astro_align(atof(v), err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m)); return;
        }
        respond_json(fd, "{\"ok\":true}"); return;
    }
    if (!strcmp(path, "/api/astro/track")) {
        char on[8]="", rate[8]="", half[8]="", err[160]="", out[256];
        if (!param(qs,"on",on,sizeof on) && body) param(body,"on",on,sizeof on);
        if (!param(qs,"rate",rate,sizeof rate) && body) param(body,"rate",rate,sizeof rate);
        if (!param(qs,"half",half,sizeof half) && body) param(body,"half",half,sizeof half);
        if (astro_track((on[0]=='1'||!strcmp(on,"true")) ? 1 : 0,
                        rate[0] ? atoi(rate) : 0,
                        (half[0]=='1'||!strcmp(half,"true")) ? 1 : 0,
                        err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m)); return;
        }
        respond_json(fd, "{\"ok\":true}"); return;
    }
    if (!strcmp(path, "/api/astro")) {
        char out[512];
        astro_status_json(out, sizeof out);
        respond_json(fd, out); return;
    }

    /* --------------------------------------------------- free program (283) */

    /* Fly the head with the jog, capture its pose as a keyframe, repeat, then
     * play. Keyframes are held server-side between captures. */
    if (!strcmp(path, "/api/prog/plc/key")) {
        char v[16]="", err[160]="", out[256];
        double t;
        if (!param(qs,"t",v,sizeof v) && body) param(body,"t",v,sizeof v);
        t = atof(v);
        { int n = prog_plc_add_key(t, err, sizeof err);
          if (n < 0) { char m[256]; json_escape(err, out, sizeof out);
              snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
              respond(fd, 409, "application/json", m, strlen(m)); return; }
          snprintf(out, sizeof out, "{\"ok\":true,\"keys\":%d}", n);
          respond_json(fd, out); return; }
    }
    if (!strcmp(path, "/api/prog/plc/clear")) {
        prog_plc_clear();
        respond_json(fd, "{\"ok\":true,\"keys\":0}"); return;
    }
    if (!strcmp(path, "/api/prog/plc")) {
        char pi[16]="", pc[16]="", hold[8]="", appt[16]="", conf[8]="";
        plc_params_t pp; char err[160]="", out[2048]; int n;
        memset(&pp, 0, sizeof pp);
        if (!param(qs,"photo_interval",pi,sizeof pi) && body) param(body,"photo_interval",pi,sizeof pi);
        if (!param(qs,"photo_count",pc,sizeof pc) && body) param(body,"photo_count",pc,sizeof pc);
        if (!param(qs,"hold",hold,sizeof hold) && body) param(body,"hold",hold,sizeof hold);
        if (!param(qs,"appointment",appt,sizeof appt) && body) param(body,"appointment",appt,sizeof appt);
        if (!param(qs,"confirm",conf,sizeof conf) && body) param(body,"confirm",conf,sizeof conf);
        /* Pull the captured keyframes back out via the module's own store. */
        pp.n_keys = prog_plc_key_count_export(pp.keys, PLC_MAX_KEYS);
        pp.photo_interval_s = atof(pi);
        pp.photo_count = pc[0] ? atol(pc) : -1;
        pp.hold = (hold[0]=='1'||!strcmp(hold,"true")) ? 1 : 0;
        pp.appointment_unix = appt[0] ? atol(appt) : 0;
        if (!(conf[0]=='1'||!strcmp(conf,"true"))) {
            char frame[2048], fesc[3072], m[3300];
            if (prog_check_plc(&pp, err, sizeof err) != 0) {
                json_escape(err, out, sizeof out);
                snprintf(m, sizeof m, "{\"ok\":false,\"valid\":false,\"error\":\"%s\"}", out);
                respond(fd, 200, "application/json", m, strlen(m)); return;
            }
            prog_plc_preview(&pp, frame, sizeof frame);
            json_escape(frame, fesc, sizeof fesc);
            snprintf(m, sizeof m, "{\"ok\":true,\"valid\":true,\"preview\":true,\"keys\":%d,\"frame\":\"%s\"}", pp.n_keys, fesc);
            respond(fd, 200, "application/json", m, strlen(m)); return;
        }
        if (prog_start_plc(&pp, err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m)); return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"prog\":");
        n += prog_status_json(out+n, (int)sizeof out - n);
        snprintf(out+n, sizeof out - n, "}");
        respond_json(fd, out); return;
    }

    /* ---------------------------------------------------- path-lapse (272) */

    /* A motion timelapse: fly the head, capture a waypoint with the frames to
     * shoot on the leg INTO it, repeat (2..8 points), then run. Same server-held
     * capture-then-play shape as the free program; same 272 opcode as timelapse
     * so it reports on the identical step-4 poll. Moves motors -> confirm gated. */
    if (!strcmp(path, "/api/prog/pathlapse/wp")) {
        char v[16]="", err[160]="", out[256]; long pc;
        if (!param(qs,"count",v,sizeof v) && body) param(body,"count",v,sizeof v);
        /* -1 (or "inf") = shoot for the whole leg; otherwise a positive count. */
        pc = (v[0]=='\0') ? -1 : (!strcmp(v,"inf") ? -1 : atol(v));
        { int n = prog_pathlapse_add_wp(pc, err, sizeof err);
          if (n < 0) { char m[256]; json_escape(err, out, sizeof out);
              snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
              respond(fd, 409, "application/json", m, strlen(m)); return; }
          snprintf(out, sizeof out, "{\"ok\":true,\"waypoints\":%d}", n);
          respond_json(fd, out); return; }
    }
    if (!strcmp(path, "/api/prog/pathlapse/clear")) {
        prog_pathlapse_clear();
        respond_json(fd, "{\"ok\":true,\"waypoints\":0}"); return;
    }
    if (!strcmp(path, "/api/prog/pathlapse")) {
        char iv[24]="", conf[8]="";
        pathlapse_params_t pp; char err[160]="", out[2048]; int n;
        memset(&pp, 0, sizeof pp);
        if (!param(qs,"interval",iv,sizeof iv) && body) param(body,"interval",iv,sizeof iv);
        if (!param(qs,"confirm",conf,sizeof conf) && body) param(body,"confirm",conf,sizeof conf);
        pp.interval_s = atof(iv);
        pp.n_wp = prog_pathlapse_wp_export(pp.wp, PATHLAPSE_MAX_WP);
        if (!(conf[0]=='1'||!strcmp(conf,"true"))) {
            char frame[2048], fesc[3072], m[3300];
            if (prog_check_pathlapse(&pp, err, sizeof err) != 0) {
                json_escape(err, out, sizeof out);
                snprintf(m, sizeof m, "{\"ok\":false,\"valid\":false,\"error\":\"%s\"}", out);
                respond(fd, 200, "application/json", m, strlen(m)); return;
            }
            prog_pathlapse_preview(&pp, frame, sizeof frame);
            json_escape(frame, fesc, sizeof fesc);
            snprintf(m, sizeof m, "{\"ok\":true,\"valid\":true,\"preview\":true,\"waypoints\":%d,\"frame\":\"%s\"}", pp.n_wp, fesc);
            respond(fd, 200, "application/json", m, strlen(m)); return;
        }
        if (prog_start_pathlapse(&pp, err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m)); return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"prog\":");
        n += prog_status_json(out+n, (int)sizeof out - n);
        snprintf(out+n, sizeof out - n, "}");
        respond_json(fd, out); return;
    }

    /* ---------------------------------------------------------- sun (277) */

    /* A scheduled sunrise/sunset lapse: the head does the solar GOTO and parks.
     * The window is clamped to now ±1h by the head, so the client's clock is the
     * authority for the pre-flight range check (the device clock is not UTC).
     * The UI passes `now` (its own unix time). GET/POST-without-confirm previews
     * the exact 277 frame; confirm=1 sends it. Moves motors -> confirm gated. */
    if (!strcmp(path, "/api/prog/sun")) {
        char set[8]="", iv[24]="", st[24]="", en[24]="", nw[24]="", conf[8]="";
        sun_params_t sp; char err[160]="", out[512]; long now = 0; int n;
        memset(&sp, 0, sizeof sp);
        if (!param(qs,"sunset",set,sizeof set) && body) param(body,"sunset",set,sizeof set);
        if (!param(qs,"interval",iv,sizeof iv) && body) param(body,"interval",iv,sizeof iv);
        if (!param(qs,"start",st,sizeof st) && body) param(body,"start",st,sizeof st);
        if (!param(qs,"end",en,sizeof en) && body) param(body,"end",en,sizeof en);
        if (!param(qs,"now",nw,sizeof nw) && body) param(body,"now",nw,sizeof nw);
        if (!param(qs,"confirm",conf,sizeof conf) && body) param(body,"confirm",conf,sizeof conf);
        sp.sunset = (set[0]=='1'||!strcmp(set,"true")||!strcmp(set,"sunset")) ? 1 : 0;
        sp.interval_s = atof(iv);
        sp.start_unix = atol(st);
        sp.end_unix   = atol(en);
        now = nw[0] ? atol(nw) : 0;
        if (!(conf[0]=='1'||!strcmp(conf,"true"))) {
            char frame[512], fesc[1024], m[1400];
            if (prog_check_sun(&sp, now, err, sizeof err) != 0) {
                json_escape(err, out, sizeof out);
                snprintf(m, sizeof m, "{\"ok\":false,\"valid\":false,\"error\":\"%s\"}", out);
                respond(fd, 200, "application/json", m, strlen(m)); return;
            }
            prog_sun_preview(&sp, frame, sizeof frame);
            json_escape(frame, fesc, sizeof fesc);
            snprintf(m, sizeof m, "{\"ok\":true,\"valid\":true,\"preview\":true,\"frame\":\"%s\"}", fesc);
            respond(fd, 200, "application/json", m, strlen(m)); return;
        }
        /* Range-check with the CLIENT clock before committing (start_sun skips
         * the now-check because the device clock cannot be trusted for it). */
        if (prog_check_sun(&sp, now, err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m)); return;
        }
        if (prog_start_sun(&sp, err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m)); return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"prog\":");
        n += prog_status_json(out+n, (int)sizeof out - n);
        snprintf(out+n, sizeof out - n, "}");
        respond_json(fd, out); return;
    }

    /* -------------------------------------------------- holy grail (305) */

    /* Configure the day->night exposure ramp. NOT a run: it uploads the target
     * brightness curve and the three axis ranges the head applies during a
     * timelapse/path-lapse, and the ramp meters through the external Optical
     * Matrix Sensor Module accessory (docs/APP-FEATURES.md 3.7). No motor and no
     * shutter, so there is no confirm gate -- but the axis-value and priority
     * encodings are inferred, so a POST without confirm returns the exact 305
     * batch to read first; confirm=1 uploads it. `/off` disables the ramp;
     * `/status` reports the enable flag and the last runtime brightness. */
    if (!strcmp(path, "/api/prog/grail")) {
        char en[8]="", pr[32]="", isoen[8]="", isolo[16]="", isohi[16]="",
             fen[8]="", flo[16]="", fhi[16]="", sen[8]="", sh[64]="",
             curve[2048]="", conf[8]="";
        grail_params_t gp; char err[160]="", out[512]; int n;
        memset(&gp, 0, sizeof gp);
        if (!param(qs,"enable",en,sizeof en) && body) param(body,"enable",en,sizeof en);
        if (!param(qs,"priority",pr,sizeof pr) && body) param(body,"priority",pr,sizeof pr);
        if (!param(qs,"iso_en",isoen,sizeof isoen) && body) param(body,"iso_en",isoen,sizeof isoen);
        if (!param(qs,"iso_lo",isolo,sizeof isolo) && body) param(body,"iso_lo",isolo,sizeof isolo);
        if (!param(qs,"iso_hi",isohi,sizeof isohi) && body) param(body,"iso_hi",isohi,sizeof isohi);
        if (!param(qs,"f_en",fen,sizeof fen) && body) param(body,"f_en",fen,sizeof fen);
        if (!param(qs,"f_lo",flo,sizeof flo) && body) param(body,"f_lo",flo,sizeof flo);
        if (!param(qs,"f_hi",fhi,sizeof fhi) && body) param(body,"f_hi",fhi,sizeof fhi);
        if (!param(qs,"s_en",sen,sizeof sen) && body) param(body,"s_en",sen,sizeof sen);
        if (!param(qs,"shutter",sh,sizeof sh) && body) param(body,"shutter",sh,sizeof sh);
        if (!param(qs,"curve",curve,sizeof curve) && body) param(body,"curve",curve,sizeof curve);
        if (!param(qs,"confirm",conf,sizeof conf) && body) param(body,"confirm",conf,sizeof conf);

        gp.enable = (en[0]=='1'||!strcmp(en,"true")) ? 1 : 0;
        if (pr[0] &&                                     /* "a,b,c" -> opt-in */
            sscanf(pr, "%d,%d,%d", &gp.priority[0], &gp.priority[1], &gp.priority[2]) == 3)
            gp.set_priority = 1;
        gp.iso_en = (isoen[0]=='1'||!strcmp(isoen,"true")) ? 1 : 0;
        gp.iso_lo = isolo[0] ? atol(isolo) : 0;
        gp.iso_hi = isohi[0] ? atol(isohi) : 0;
        gp.f_en = (fen[0]=='1'||!strcmp(fen,"true")) ? 1 : 0;
        gp.f_lo = atof(flo); gp.f_hi = atof(fhi);
        gp.s_en = (sen[0]=='1'||!strcmp(sen,"true")) ? 1 : 0;
        if (sh[0]) sscanf(sh, "%d,%d,%d,%d", &gp.s[0], &gp.s[1], &gp.s[2], &gp.s[3]);
        {   /* curve: "dmin/ev,dmin/ev,..." */
            char *save = NULL, *tok;
            gp.n_nodes = 0;
            for (tok = strtok_r(curve, ",", &save); tok && gp.n_nodes < GRAIL_MAX_NODES;
                 tok = strtok_r(NULL, ",", &save)) {
                int dm; double ev;
                if (sscanf(tok, "%d/%lf", &dm, &ev) == 2) {
                    gp.node[gp.n_nodes].dmin = dm;
                    gp.node[gp.n_nodes].ev = ev;
                    gp.n_nodes++;
                }
            }
        }
        /* The line-0 anchor is mandatory; default it if the client sent none. */
        if (gp.n_nodes == 0) { gp.node[0].dmin = 0; gp.node[0].ev = 0; gp.n_nodes = 1; }

        if (!(conf[0]=='1'||!strcmp(conf,"true"))) {
            char frame[3072], fesc[4096], m[4300];
            if (prog_grail_check(&gp, err, sizeof err) != 0) {
                json_escape(err, out, sizeof out);
                snprintf(m, sizeof m, "{\"ok\":false,\"valid\":false,\"error\":\"%s\"}", out);
                respond(fd, 200, "application/json", m, strlen(m)); return;
            }
            prog_grail_preview(&gp, frame, sizeof frame);
            json_escape(frame, fesc, sizeof fesc);
            /* `inferred` flags that the axis-value and priority encodings are a
             * best guess (like panorama) so the UI can say so before it sends. */
            snprintf(m, sizeof m,
                "{\"ok\":true,\"valid\":true,\"preview\":true,\"inferred\":true,"
                "\"nodes\":%d,\"frame\":\"%s\"}", gp.n_nodes, fesc);
            respond(fd, 200, "application/json", m, strlen(m)); return;
        }
        if (prog_grail_apply(&gp, err, sizeof err) != 0) {
            char m[256]; json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m)); return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"enabled\":%s}",
                     prog_grail_enabled() ? "true" : "false");
        respond(fd, 200, "application/json", out, (size_t)n); return;
    }
    if (!strcmp(path, "/api/prog/grail/off")) {
        char err[160]="", out[256], m[256];
        if (prog_grail_disable(err, sizeof err) != 0) {
            json_escape(err, out, sizeof out);
            snprintf(m, sizeof m, "{\"ok\":false,\"error\":\"%s\"}", out);
            respond(fd, 409, "application/json", m, strlen(m)); return;
        }
        respond_json(fd, "{\"ok\":true,\"enabled\":false}"); return;
    }
    if (!strcmp(path, "/api/prog/grail/status")) {
        char br[PLINK_ARGS_MAX], bresc[PLINK_ARGS_MAX*2], out[1200];
        prog_grail_brightness(br, sizeof br);
        json_escape(br, bresc, sizeof bresc);
        snprintf(out, sizeof out, "{\"enabled\":%s,\"brightness\":\"%s\"}",
                 prog_grail_enabled() ? "true" : "false", bresc);
        respond_json(fd, out); return;
    }

    /* What the UI is allowed to send, so it can grey out what it cannot do
     * instead of discovering it with a 403 mid-gesture. */
    if (!strcmp(path, "/api/link/opcodes")) {
        char out[8192];
        int n = 0; size_t i;
        n += snprintf(out + n, sizeof out - n, "{\"opcodes\":[");
        for (i = 0; i < OPCODE_COUNT && n < (int)sizeof out - 256; i++) {
            n += snprintf(out + n, sizeof out - n,
                "%s{\"cmd\":%d,\"name\":\"%s\",\"sub\":%d,\"moves\":%s,\"note\":\"%s\"}",
                i ? "," : "", OPCODES[i].cmd, OPCODES[i].name, OPCODES[i].sub,
                OPCODES[i].moves ? "true" : "false", OPCODES[i].note);
        }
        n += snprintf(out + n, sizeof out - n, "]}");
        respond_json(fd, out);
        return;
    }

    if (!strcmp(path, "/api/state")) {
        char status[32], log[4096], logesc[8192], result[4096], raw[1024], out[16384];
        double alt = 0, az = 0;
        int aligned = 0, tracking = 0, want_mount = 0, mount_blocked = 0;
        long elapsed = 0;
        read_file(JOB_STATUS, status, sizeof status);
        if (!status[0]) snprintf(status, sizeof status, "idle");
        { char *e = status + strlen(status);
          while (e > status && (e[-1]=='\n'||e[-1]==' ')) *--e = 0; }
        read_tail(JOB_LOG, log, sizeof log);
        json_escape(log, logesc, sizeof logesc);
        read_file(JOB_RESULT, result, sizeof result);
        { char *e = strchr(result, '\n'); if (e) *e = 0; }
        if (!strstr(result, "\"solved\"")) snprintf(result, sizeof result, "null");

        /* DO NOT poll the mount on a timer.
         *
         * Every polaris-mount call opens a fresh TCP connection to the control
         * port; the device treats each one as an app connecting
         * (SP_EVENT_APP_CONNECT) and answers with a state push. With the page
         * polling on an interval that nagged the Benro app into repeatedly
         * prompting for compass calibration -- confirmed by isolation: httpd
         * idle produced 0 connections in 15 s, four /api/state polls produced
         * exactly four. Caching only reduced the rate; the fix is to not do it
         * unless the user actually asks.
         *
         * Mount fields are populated ONLY for /api/state?mount=1, which the
         * page issues from an explicit Refresh button. */
        /* Alignment/mode always come from the LOG -- free, no connections. */
        { int lmode, ltrack;
          if (log_state(&lmode, &ltrack) && ltrack >= 0) note_track(ltrack);
          (void)lmode;
          aligned  = (g_last_track >= 0 && g_last_track != 3);
          tracking = (g_last_track == 1); }

        /* An ACTIVE read (which opens a connection) happens only when asked
         * AND only when the mount is already aligned. While it is unaligned --
         * i.e. exactly when the app is running its calibration -- connecting
         * makes the app re-prompt for compass calibration, so we refuse. */
        { char mq[8];
          if (param(qs, "mount", mq, sizeof mq) && mq[0] == '1')
              want_mount = 1; }
        if (want_mount && !may_read_mount()) {
            want_mount = 0;
            mount_blocked = 1;
        }
        if (want_mount && mount_pose(&alt, &az, raw, sizeof raw)) {
            /* a real read is authoritative -- record it for later decisions */
            { const char *t = strstr(raw, "\"track\":");
              if (t) { note_track(atoi(t + 8)); tracking = atoi(t + 8) == 1; } }
            if (strstr(raw, "\"aligned\":true")) aligned = 1;
            else if (strstr(raw, "\"aligned\":false")) aligned = 0;
        }
        { char sb[32]; long st0; read_file(JOB_START, sb, sizeof sb); st0 = atol(sb);
          elapsed = (st0 > 0 && !strcmp(status, "running")) ? (long)time(NULL) - st0 : 0; }
        snprintf(out, sizeof out,
            "{\"status\":\"%s\",\"elapsed_sec\":%ld,\"mount_read\":%s,\"mount_blocked\":%s,"
            "\"alt\":%.4f,\"az\":%.4f,"
            "\"aligned\":%s,\"tracking\":%s,\"solution\":%s,\"log\":\"%s\"}",
            status, elapsed, want_mount?"true":"false", mount_blocked?"true":"false", alt, az,
            aligned?"true":"false", tracking?"true":"false", result, logesc);
        respond_json(fd, out);
        return;
    }

    /* Focal length. GET reports what the solver will use and where it came
     * from; POST sets or clears the manual override.
     *
     * The override exists because a telescope or an adapted manual lens has no
     * electrical contacts and reports NO focal length, and a body will stamp a
     * stale value into EXIF for one. A typed-in number is better information
     * than anything we can infer, so it outranks EXIF. "auto" deletes it. */
    /* Cancel a running solve. A range search runs for minutes and there was no
     * way to stop it except waiting -- during which every other solve request
     * was refused. */
    if (!strcmp(path, "/api/cancel")) {
        char pb[32]; long jp = 0; int killed = 0;
        read_file(JOB_PID, pb, sizeof pb);
        jp = atol(pb);
        if (jp > 0 && kill((pid_t)jp, 0) == 0) {
            /* the job is a session leader (setsid), so signal the whole group
             * -- killing only the shell would leave polaris-solve running and
             * still holding the cpu. */
            kill((pid_t)-jp, SIGTERM);
            kill((pid_t)jp, SIGTERM);
            killed = 1;
        }
        set_status("failed");
        unlink(JOB_PID);
        respond_json(fd, killed ? "{\"cancelled\":true}"
                                : "{\"cancelled\":false,\"reason\":\"nothing running\"}");
        return;
    }

    /* Keep the wifi radio awake.
     *
     * The firmware powers wifi down 60 s after the last client disconnects
     * ("wifi auto off" in its own log) and unloads the driver, which takes the
     * SSID, ssh and this page with it. There is no setting for that, so the
     * workaround is to stay connected: one idle TCP connection to the control
     * port keeps the client count above zero.
     *
     * OFF by default and deliberately so -- it costs battery, and the device
     * counts our connection as an app connecting, which is what makes the Benro
     * app demand a compass calibration while the mount is unaligned. The helper
     * therefore waits for alignment before it connects. */
    /* Home-network (station) configuration and control.
     *
     * HOW THE PASSWORD IS HANDLED, because this is the one place the page
     * carries a real secret:
     *   - accepted ONLY in the request BODY, never the query string, which is
     *     the part that ends up in browser history and proxy logs;
     *   - never written to any log and never echoed back -- GET returns the
     *     SSID and whether a key is configured, never the key;
     *   - never placed on a command line, because argv is world-readable
     *     through /proc; it goes to wpa_passphrase on stdin via a 0600 file
     *     that is unlinked immediately;
     *   - only the PSK HASH is stored. wpa_passphrase helpfully repeats the
     *     plaintext back as a "#psk=" comment; that line is stripped.
     *
     * What this does NOT do is make the transport secure: the page is plain
     * HTTP. The link itself is WPA2-encrypted between your device and the
     * Polaris AP, so it is not in the clear over the air, but anything already
     * on that AP could read it. The ssh path (setup-wifi.sh) avoids that. */
    if (!strcmp(path, "/api/wifi")) {
        char out[1024];
        const char *WD = "/app/sd/polaris-wifi";
        char action[16] = "", ssid[128] = "", psk[128] = "";

        if (!strcmp(method, "POST") || !strcmp(method, "PUT")) {
            if (body) {
                param_body(body, "action", action, sizeof action);
                param_body(body, "ssid", ssid, sizeof ssid);
                param_body(body, "psk", psk, sizeof psk);
            }
            if (!action[0]) param(qs, "action", action, sizeof action);

            if (!strcmp(action, "save")) {
                char cmd[768], tmp[64] = "/tmp/.wpa-psk";
                FILE *f;
                size_t i;
                /* An SSID goes onto wpa_passphrase's command line, so refuse
                 * anything that could break out of the quoting. */
                for (i = 0; ssid[i]; i++) {
                    if (ssid[i] == '\'' || ssid[i] == '"' || ssid[i] == '\\' ||
                        ssid[i] == '`'  || ssid[i] == '$' || ssid[i] == '\n') {
                        respond_json(fd, "{\"ok\":false,\"error\":\"that SSID contains a "
                            "character this cannot safely quote (quote, backslash, backtick "
                            "or $) -- use the ssh setup script instead\"}");
                        memset(psk, 0, sizeof psk);
                        return;
                    }
                }
                if (!ssid[0] || strlen(psk) < 8) {
                    respond_json(fd, "{\"ok\":false,\"error\":\"need an SSID and a password "
                        "of at least 8 characters\"}");
                    memset(psk, 0, sizeof psk);
                    return;
                }
                f = fopen(tmp, "w");
                if (!f) { respond_json(fd, "{\"ok\":false,\"error\":\"cannot stage the key\"}");
                          memset(psk, 0, sizeof psk); return; }
                chmod(tmp, 0600);
                fputs(psk, f);
                fclose(f);
                memset(psk, 0, sizeof psk);          /* out of our memory too */

                snprintf(cmd, sizeof cmd,
                    "cd %s && { echo ctrl_interface=/var/run/wpa_supplicant; "
                    "echo update_config=1; "
                    "./wpa_passphrase '%s' < %s | grep -v '^[[:space:]]*#psk='; } "
                    "> .wpa.tmp 2>/dev/null && mv .wpa.tmp wpa.conf && chmod 600 wpa.conf",
                    WD, ssid, tmp);
                { int rc = system(cmd);
                  unlink(tmp);
                  if (rc != 0) {
                      respond_json(fd, "{\"ok\":false,\"error\":\"wpa_passphrase failed\"}");
                      return;
                  } }
                respond_json(fd, "{\"ok\":true,\"saved\":true}");
                return;
            }
            if (!strcmp(action, "autojoin_on") || !strcmp(action, "autojoin_off")) {
                if (!strcmp(action, "autojoin_on")) {
                    FILE *f = fopen("/app/sd/polaris-wifi/autojoin", "w");
                    if (f) { fputs("1\n", f); fclose(f); }
                } else unlink("/app/sd/polaris-wifi/autojoin");
                respond_json(fd, "{\"ok\":true}");
                return;
            }
            if (!strcmp(action, "up") || !strcmp(action, "down")) {
                char cmd[256], res[2048], esc[4096];
                /* "up" runs the SAME script boot uses, so what you test from
                 * the page is exactly what happens at power-on -- no second
                 * code path to drift out of agreement. */
                if (!strcmp(action, "up"))
                    snprintf(cmd, sizeof cmd, "timeout -t 110 %s/polaris-autojoin.sh 2>&1 | tail -20", WD);
                else
                    snprintf(cmd, sizeof cmd, "timeout -t 75 %s/polaris-apsta.sh down 2>&1", WD);
                run_capture(cmd, res, sizeof res);
                json_escape(res, esc, sizeof esc);
                snprintf(out, sizeof out, "{\"ok\":true,\"action\":\"%s\",\"log\":\"%s\"}",
                         action, esc);
                respond_json(fd, out);
                return;
            }
        }

        /* status: SSID yes, key never */
        {
            char apip[64] = "", staip[64] = "", cssid[160] = "", res[256];
            int haskey = 0, autojoin = 0;
            if (run_capture("ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\\([0-9.]*\\).*/\\1/p'",
                            res, sizeof res) == 0) {
                snprintf(apip, sizeof apip, "%s", res);
                { char *e = apip; while (*e) { if (*e=='\n'||*e=='\r') { *e=0; break; } e++; } }
            }
            if (run_capture("ifconfig wlan1 2>/dev/null | sed -n 's/.*inet addr:\\([0-9.]*\\).*/\\1/p'",
                            res, sizeof res) == 0) {
                snprintf(staip, sizeof staip, "%s", res);
                { char *e = staip; while (*e) { if (*e=='\n'||*e=='\r') { *e=0; break; } e++; } }
            }
            if (run_capture("sed -n 's/^[[:space:]]*ssid=\"\\(.*\\)\"/\\1/p' "
                            "/app/sd/polaris-wifi/wpa.conf 2>/dev/null | head -1",
                            res, sizeof res) == 0) {
                snprintf(cssid, sizeof cssid, "%s", res);
                { char *e = cssid; while (*e) { if (*e=='\n'||*e=='\r') { *e=0; break; } e++; } }
            }
            { FILE *c = fopen("/app/sd/polaris-wifi/wpa.conf", "r");
              if (c) { haskey = 1; fclose(c); } }
            { FILE *aj = fopen("/app/sd/polaris-wifi/autojoin", "r");
              if (aj) { autojoin = 1; fclose(aj); } }
            snprintf(out, sizeof out,
                "{\"ok\":true,\"ap_ip\":\"%s\",\"sta_ip\":\"%s\","
                "\"ssid\":\"%s\",\"configured\":%s,\"autojoin\":%s}",
                apip, staip, cssid, haskey ? "true" : "false",
                autojoin ? "true" : "false");
            respond_json(fd, out);
        }
        return;
    }

    if (!strcmp(path, "/api/keepwifi")) {
        char out[320]; int on = 0, running = 0, forced = 0;
        if (!strcmp(method, "POST") || !strcmp(method, "PUT")) {
            char v[16] = "";
            if (!param(qs, "on", v, sizeof v) && body) param(body, "on", v, sizeof v);
            if (v[0] == '1' || !strcmp(v, "true") || !strcmp(v, "force")) {
                FILE *f = fopen(KEEPWIFI_FLAG, "w");
                if (f) { fputs("1\n", f); fclose(f); }
                /* "force" means connect without waiting for alignment. It is a
                 * deliberate choice with a real consequence -- registering while
                 * the mount is unaligned is what makes the Benro app ask for a
                 * compass calibration -- so it is a separate, explicit flag
                 * rather than a quietly relaxed default. */
                if (!strcmp(v, "force")) {
                    FILE *ff = fopen(KEEPWIFI_FORCE, "w");
                    if (ff) { fputs("1\n", ff); fclose(ff); }
                } else {
                    unlink(KEEPWIFI_FORCE);
                }
                /* Launch unconditionally; the script refuses to double-start
                 * using a pidfile. Do NOT guard with `ps | grep` here: the
                 * guard runs in a subshell whose own command line contains the
                 * pattern, so it matches itself and never starts anything. */
                { char cmd[320];
                  snprintf(cmd, sizeof cmd,
                     "setsid %s/wifi-keepalive.sh </dev/null >/dev/null 2>&1 &", g_astro);
                  if (system(cmd) == -1) { /* reported via `running` below */ } }
            } else {
                char pb[32]; long kp;
                unlink(KEEPWIFI_FLAG);
                unlink(KEEPWIFI_FORCE);
                read_file(KEEPWIFI_PID, pb, sizeof pb);
                kp = atol(pb);
                if (kp <= 0 || kill((pid_t)kp, 0) != 0) kp = (long)find_proc("wifi-keepalive.sh");
                if (kp > 0) {
                    int w;
                    /* it is a session leader, so signal the group -- killing
                     * only the script would leave the nc holding the socket,
                     * and the whole point is to release it */
                    kill((pid_t)-kp, SIGTERM);
                    kill((pid_t)kp, SIGTERM);
                    /* VERIFY, do not assume. Reporting "off" while the helper
                     * is still holding the connection is worse than useless --
                     * the wifi would stay awake and the page would say it is
                     * not. Escalate if TERM was not enough. */
                    for (w = 0; w < 20 && kill((pid_t)kp, 0) == 0; w++) usleep(100000);
                    if (kill((pid_t)kp, 0) == 0) {
                        kill((pid_t)-kp, SIGKILL);
                        kill((pid_t)kp, SIGKILL);
                        for (w = 0; w < 10 && kill((pid_t)kp, 0) == 0; w++) usleep(100000);
                    }
                    /* duplicates can exist -- earlier races left two running */
                    { pid_t o; int guard = 0;
                      while ((o = find_proc("wifi-keepalive.sh")) > 0 && guard++ < 8) {
                          kill(-o, SIGTERM); kill(o, SIGTERM);
                          usleep(300000);
                          if (kill(o, 0) == 0) { kill(-o, SIGKILL); kill(o, SIGKILL); usleep(200000); }
                      } }
                }
                unlink(KEEPWIFI_PID);
            }
        }
        { FILE *f = fopen(KEEPWIFI_FLAG, "r"); if (f) { on = 1; fclose(f); } }
        running = (find_proc("wifi-keepalive.sh") > 0);
        { FILE *ff = fopen(KEEPWIFI_FORCE, "r"); if (ff) { forced = 1; fclose(ff); } }
        snprintf(out, sizeof out,
                 "{\"ok\":true,\"enabled\":%s,\"running\":%s,\"aligned\":%s,\"forced\":%s}",
                 on ? "true" : "false", running ? "true" : "false",
                 may_read_mount() ? "true" : "false", forced ? "true" : "false");
        respond_json(fd, out);
        return;
    }

    /* Guiding: status, drift history, and start/stop.
     *
     * The drift history is parsed out of the guider's own log rather than kept
     * in a second place that could disagree with it. Lines look like
     *     14:02:11 drift 61.2" (dx=1.2 dy=3.4 px) exceeds 60"
     *     14:02:41 drift 12.3" (dx=0.2 dy=0.7 px) -- within tolerance
     * and a correction is logged separately as "correcting: goto-radec ...".
     */
    if (!strcmp(path, "/api/guide")) {
        char out[16384], pts[12288], tail[3072], tailesc[6144];
        int running = 0, npt = 0;
        double last = -1;
        pts[0] = 0;

        if (!strcmp(method, "POST") || !strcmp(method, "PUT")) {
            char v[16] = "";
            if (!param(qs, "on", v, sizeof v) && body) param(body, "on", v, sizeof v);
            if (v[0] == '1' || !strcmp(v, "true")) {
                /* Guiding needs something to guide ON. Without a solved
                 * position there is no anchor, so refuse rather than start a
                 * process that will immediately give up. */
                double ra, dec;
                if (!last_solution(&ra, &dec)) {
                    respond_json(fd, "{\"ok\":false,\"error\":"
                        "\"nothing solved yet -- solve a frame first so there is a target to guide on\"}");
                    return;
                }
                if (!may_read_mount()) {
                    respond_json(fd, "{\"ok\":false,\"error\":"
                        "\"mount is not aligned -- guiding would move it against an unknown pointing\"}");
                    return;
                }
                { char cmd[640];
                  snprintf(cmd, sizeof cmd,
                     "LAT=%.6f LON=%.6f FOCAL_MM=%.0f DRY_RUN=%s INTERVAL=%s THRESH_ARCSEC=%s "
                     "setsid sh %s/polaris-guide.sh --ra %.6f --dec %.6f "
                     "</dev/null >/tmp/guide.out 2>&1 &",
                     g_lat, g_lon, g_focal,
                     getenv("GUIDE_DRY_RUN") ? getenv("GUIDE_DRY_RUN") : "0",
                     getenv("GUIDE_INTERVAL") ? getenv("GUIDE_INTERVAL") : "30",
                     getenv("GUIDE_THRESH") ? getenv("GUIDE_THRESH") : "60",
                     g_astro, ra, dec);
                  if (system(cmd) == -1) {} }
            } else {
                pid_t g; int guard = 0;
                while ((g = find_proc("polaris-guide.sh")) > 0 && guard++ < 8) {
                    kill(-g, SIGTERM); kill(g, SIGTERM);
                    usleep(300000);
                    if (kill(g, 0) == 0) { kill(-g, SIGKILL); kill(g, SIGKILL); usleep(200000); }
                }
            }
        }

        running = (find_proc("polaris-guide.sh") > 0);

        /* drift points, oldest first, from the guider's log */
        {
            FILE *f = fopen("/app/sd/polaris-guide.log", "r");
            if (f) {
                char line[512];
                char stack[64][64];
                int nst = 0;
                while (fgets(line, sizeof line, f)) {
                    const char *d = strstr(line, "drift ");
                    if (!d) continue;
                    { double val = atof(d + 6);
                      int corrected = (strstr(line, "exceeds") != NULL);
                      char hhmm[16] = "";
                      /* leading "HH:MM:SS " timestamp written by log() */
                      if (strlen(line) > 8 && line[2] == ':' && line[5] == ':') {
                          memcpy(hhmm, line, 8); hhmm[8] = 0;
                      }
                      snprintf(stack[nst % 64], 64, "{\"t\":\"%s\",\"d\":%.2f,\"c\":%s}",
                               hhmm, val, corrected ? "true" : "false");
                      nst++;
                      last = val; }
                }
                fclose(f);
                { int start = nst > 64 ? nst - 64 : 0, i;
                  for (i = start; i < nst; i++) {
                      size_t used = strlen(pts);
                      const char *one = stack[i % 64];
                      if (used + strlen(one) + 2 >= sizeof pts) break;
                      if (npt) strcat(pts, ",");
                      strcat(pts, one);
                      npt++;
                  } }
            }
        }
        read_tail("/app/sd/polaris-guide.log", tail, sizeof tail);
        json_escape(tail, tailesc, sizeof tailesc);
        snprintf(out, sizeof out,
            "{\"ok\":true,\"running\":%s,\"threshold\":%s,\"interval\":%s,"
            "\"last_drift\":%.2f,\"points\":[%s],\"log\":\"%s\"}",
            running ? "true" : "false",
            getenv("GUIDE_THRESH") ? getenv("GUIDE_THRESH") : "60",
            getenv("GUIDE_INTERVAL") ? getenv("GUIDE_INTERVAL") : "30",
            last, pts, tailesc);
        respond_json(fd, out);
        return;
    }

    if (!strcmp(path, "/api/focal")) {
        char ovr[64] = "", cache[64] = "", out[512];
        if (!strcmp(method, "POST") || !strcmp(method, "PUT")) {
            char v[64] = "";
            /* accept ?focal=... or a form body */
            if (!param(qs, "focal", v, sizeof v) && body)
                param(body, "focal", v, sizeof v);
            if (!v[0] || !strcmp(v, "auto") || !strcmp(v, "0")) {
                unlink(FOCAL_OVERRIDE_FILE);
            } else {
                double d = atof(v);
                /* Reject nonsense rather than storing it: a bad focal does not
                 * make solving slow, it makes it IMPOSSIBLE, and a silently
                 * accepted "0" or "abc" would look like the feature is on
                 * while every solve fails. */
                if (d < 1.0 || d > 100000.0) {
                    respond_json(fd, "{\"ok\":false,\"error\":\"focal must be 1-100000 mm, or auto\"}");
                    return;
                }
                { FILE *f = fopen(FOCAL_OVERRIDE_FILE, "w");
                  if (!f) { respond_json(fd, "{\"ok\":false,\"error\":\"cannot write override file\"}"); return; }
                  fprintf(f, "%.4f\n", d); fclose(f); }
            }
        }
        read_file(FOCAL_OVERRIDE_FILE, ovr, sizeof ovr);
        read_file("/app/sd/polaris-astro/last-focal", cache, sizeof cache);
        { char *e; for (e = ovr;   *e; e++) if (*e=='\n'||*e=='\r') { *e = 0; break; }
                   for (e = cache; *e; e++) if (*e=='\n'||*e=='\r') { *e = 0; break; } }
        snprintf(out, sizeof out,
                 "{\"ok\":true,\"override\":%s%s%s,\"exif_cache\":%s%s%s,\"config\":%.1f}",
                 ovr[0]?"\"":"",   ovr[0]?ovr:"null",     ovr[0]?"\"":"",
                 cache[0]?"\"":"", cache[0]?cache:"null", cache[0]?"\"":"",
                 g_focal);
        respond_json(fd, out);
        return;
    }

    /* Apply the LAST solution, without re-solving.
     *
     * Solving and applying were welded together, so a solve you liked could
     * only be used by solving again -- and the second solve might fail or land
     * differently. The result is already on disk; this pushes it.
     *
     * It applies at the FRAME'S time, not now: RA/Dec is fixed in the sky
     * frame, and the sky turns 15.041"/s. It also refuses a stale solution --
     * if the frame is minutes old the mount has probably moved and the
     * solution no longer describes where it points. */
    if (!strcmp(path, "/api/apply")) {
        double ra, dec;
        char eb[32], out[512];
        long fep = 0, age = 0, maxage = 600;
        if (!last_solution(&ra, &dec)) {
            respond_json(fd, "{\"ok\":false,\"error\":\"no solution to apply\"}");
            return;
        }
        read_file("/tmp/polaris-job.frame-epoch", eb, sizeof eb);
        fep = atol(eb);
        if (fep > 0) age = (long)time(NULL) - fep;
        { char f[8]; if (param(qs, "force", f, sizeof f) && f[0] == '1') maxage = 86400; }
        /* FAIL CLOSED ON UNKNOWN AGE. If we do not know when the frame was
         * taken we cannot know whether the mount has moved since, and applying
         * a solution that no longer describes where it points is exactly the
         * failure this guard exists to prevent. The first version skipped the
         * check when the timestamp was missing -- i.e. it was most permissive
         * precisely when it knew least. */
        if (fep <= 0 && maxage != 86400) {
            respond_json(fd, "{\"ok\":false,\"error\":\"this solution has no recorded frame time, "
                "so its age cannot be checked -- solve a fresh frame, or pass force=1 if you are "
                "certain the mount has not moved\"}");
            return;
        }
        if (fep > 0 && age > maxage) {
            snprintf(out, sizeof out,
                "{\"ok\":false,\"error\":\"that solution is %ld s old -- the mount has "
                "probably moved since, so it no longer describes where it points. "
                "Solve a fresh frame.\",\"age_sec\":%ld}", age, age);
            respond_json(fd, out);
            return;
        }
        if (!may_read_mount()) {
            respond_json(fd, "{\"ok\":false,\"error\":\"mount is not aligned\"}");
            return;
        }
        {
            char cmd[640], res[1024], ub[64] = "";
            if (fep > 0) {
                time_t t = (time_t)(fep + tz_offset_sec());
                struct tm *g = gmtime(&t);
                if (g) { char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", g);
                         snprintf(ub, sizeof ub, "--utc %s", ts); }
            }
            /* bounded: an unaligned mount never answers 518/530, and
             * polaris-mount then sits out its own timeout while this request
             * hangs. Observed hanging a smoke test for the full client timeout. */
            snprintf(cmd, sizeof cmd,
                "timeout -t 25 %s/polaris-mount --lat %.6f --lon %.6f %s align "
                "--solved-ra %.6f --solved-dec %.6f 2>&1",
                g_astro, g_lat, g_lon, ub, ra, dec);
            if (run_capture(cmd, res, sizeof res) == 0) {
                char esc[2048];
                json_escape(res, esc, sizeof esc);
                snprintf(out, sizeof out,
                    "{\"ok\":true,\"age_sec\":%ld,\"result\":\"%s\"}", age, esc);
            } else {
                snprintf(out, sizeof out,
                    "{\"ok\":false,\"error\":\"align failed\",\"age_sec\":%ld}", age);
            }
            respond_json(fd, out);
        }
        return;
    }

    if (!strcmp(path, "/api/solve")) {
        char a[16], m[16];
        int apply = param(qs, "apply", a, sizeof a) && a[0] == '1';
        int mode = 0;
        if (param(qs, "mode", m, sizeof m)) {
            if (!strcmp(m, "latest")) mode = 1;
            else if (!strcmp(m, "wait")) mode = 2;
            else if (!strcmp(m, "capture")) mode = 3;   /* fire the shutter ourselves */
        }
        if (start_solve(apply, mode) == 0) {
            /* Do not lie by omission. This used to answer started:true whether
             * or not anything started, so a solve requested while a long one
             * was still running looked like a solve that failed. A range
             * search takes ~148 s, so that window is minutes wide. */
            char busy[192], sb[32]; long st0 = 0, el = 0;
            read_file(JOB_START, sb, sizeof sb);
            st0 = atol(sb);
            if (st0 > 0) el = (long)time(NULL) - st0;
            snprintf(busy, sizeof busy,
                     "{\"started\":false,\"reason\":\"a solve is already running\","
                     "\"elapsed_sec\":%ld}", el);
            respond_json(fd, busy);
            return;
        }
        respond_json(fd, "{\"started\":true}");
        return;
    }

    if (handle_alpaca(fd, method, path, qs, body)) return;

    respond(fd, 404, "text/plain", "not found\n", 10);
}

/* ALPACA DISCOVERY (UDP 32227).
 *
 * Clients broadcast the ASCII string "alpacadiscovery1" and expect
 * {"AlpacaPort":<n>} back. Without it every client has to be told our address
 * by hand, which is most of the friction in setting one up. */
#define ALPACA_DISCOVERY_PORT 32227

static int make_discovery_socket(void) {
    int fd, one = 1;
    struct sockaddr_in a;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);      /* children must not hold the listener */
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(ALPACA_DISCOVERY_PORT);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

static void handle_discovery(int fd, int http_port) {
    char buf[256], reply[64];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) return;
    buf[n] = 0;
    if (!strstr(buf, "alpacadiscovery")) return;
    n = snprintf(reply, sizeof reply, "{\"AlpacaPort\":%d}", http_port);
    sendto(fd, reply, (size_t)n, 0, (struct sockaddr *)&from, fl);
}

static int make_listener(int port) {
    int fd, one = 1;
    struct sockaddr_in a;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);      /* children must not hold the listener */
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    return fd;
}

int main(int argc, char **argv) {
    int port = PORT_DEFAULT, sfd, i;
    int lx_port = 10001, lxfd = -1, dfd = -1, discovery = 1;
    struct sockaddr_in a;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i+1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lat") && i+1 < argc) { g_lat = atof(argv[++i]); g_have_pos = 1; }
        else if (!strcmp(argv[i], "--lon") && i+1 < argc) { g_lon = atof(argv[++i]); g_have_pos = 1; }
        else if (!strcmp(argv[i], "--focal") && i+1 < argc) g_focal = atof(argv[++i]);
        else if (!strcmp(argv[i], "--astro") && i+1 < argc) g_astro = argv[++i];
        else if (!strcmp(argv[i], "--lx200-port") && i+1 < argc) lx_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-discovery")) discovery = 0;
        else if (!strcmp(argv[i], "--mount-host") && i+1 < argc) g_mount_host = argv[++i];
        else if (!strcmp(argv[i], "--mount-port") && i+1 < argc) g_mount_port = atoi(argv[++i]);
        else {
            fprintf(stderr,
                "usage: %s [--port N] --lat DEG --lon DEG [--focal MM] [--astro DIR]\n", argv[0]);
            return 2;
        }
    }
    if (!g_have_pos) { fprintf(stderr, "--lat and --lon are required (the solver hint needs them)\n"); return 2; }

    signal(SIGPIPE, SIG_IGN);
    /* NOTE: do NOT set SIGCHLD to SIG_IGN -- it makes popen()/pclose() and
     * system() unable to reap their own children, so every command we run
     * would look like it failed. start_solve() double-forks instead, which
     * orphans the job to init and leaves no zombie to reap. */

    (void)a;
    sfd = make_listener(port);
    if (sfd < 0) { perror("bind http"); return 1; }
    if (lx_port > 0) {
        lxfd = make_listener(lx_port);
        if (lxfd < 0) fprintf(stderr, "could not bind LX200 port %d (continuing)\n", lx_port);
    }
    if (discovery) {
        dfd = make_discovery_socket();
        if (dfd < 0) fprintf(stderr, "could not bind Alpaca discovery :%d (continuing)\n",
                             ALPACA_DISCOVERY_PORT);
    }

    if (access(JOB_STATUS, F_OK) != 0) set_status("idle");
    load_track();
    fprintf(stderr, "polaris-httpd on :%d  (lat %.5f lon %.5f focal %.0fmm, mount %s:%d)\n",
            port, g_lat, g_lon, g_focal, g_mount_host, g_mount_port);
    if (lxfd >= 0) fprintf(stderr, "LX200 on :%d\n", lx_port);
    if (dfd  >= 0) fprintf(stderr, "Alpaca discovery on udp:%d\n", ALPACA_DISCOVERY_PORT);

    for (;;) {
        fd_set rf;
        int mx = sfd, lfd;
        struct timeval tv;
        FD_ZERO(&rf);
        FD_SET(sfd, &rf);
        if (lxfd >= 0) { FD_SET(lxfd, &rf); if (lxfd > mx) mx = lxfd; }
        if (dfd  >= 0) { FD_SET(dfd,  &rf); if (dfd  > mx) mx = dfd;  }
        /* The control link rides the same loop: its socket joins the read set
         * so pushed telemetry is absorbed the moment it lands, and the timeout
         * below means the reconnect backoff and re-registration still run when
         * nothing at all is happening. Without the timeout a link that dropped
         * while the UI was closed would never come back -- select() would sit
         * forever waiting on an fd that no longer exists. */
        lfd = plink_fd();
        if (lfd >= 0) { FD_SET(lfd, &rf); if (lfd > mx) mx = lfd; }
        /* Sleep until the next thing that needs doing. While an axis is
         * moving that is the 50 ms fast-jog repeat (or, sooner, the moment a
         * lease expires and the axis must be STOPPED -- being late with a stop
         * is the one deadline here with a physical consequence). Otherwise
         * 250 ms is plenty and keeps an idle server idle. */
        {
            double due = jog_next_deadline_ms();
            double pdue = prog_next_deadline_ms();
            double adue = astro_next_deadline_ms();
            if (pdue >= 0 && (due < 0 || pdue < due)) due = pdue;
            if (adue >= 0 && (due < 0 || adue < due)) due = adue;
            double ms = (due >= 0 && due < 250.0) ? due : 250.0;
            if (ms < 1.0) ms = 1.0;
            tv.tv_sec = 0;
            tv.tv_usec = (long)(ms * 1000.0);
        }
        if (select(mx + 1, &rf, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        link_reconcile();
        plink_pump();
        telemetry_poll();
        jog_tick();
        prog_tick();
        astro_tick();
        if (FD_ISSET(sfd, &rf)) {
            int c = accept(sfd, NULL, NULL);
            /* CLOSE-ON-EXEC. Anything we launch with system() forks a shell
             * that would otherwise inherit this socket, and a LONG-LIVED child
             * (the wifi keepalive runs forever) then holds the client
             * connection open so the HTTP response never completes and the
             * browser hangs on a request that was already answered. */
            if (c >= 0) fcntl(c, F_SETFD, FD_CLOEXEC);
            if (c >= 0) { handle(c); close(c); }
        }
        if (lxfd >= 0 && FD_ISSET(lxfd, &rf)) {
            int c = accept(lxfd, NULL, NULL);
            if (c >= 0) fcntl(c, F_SETFD, FD_CLOEXEC);
            if (c >= 0) {
                /* LX200 clients hold the socket open and poll once a second, so
                 * the session runs in a child; handling it inline would block
                 * the HTTP server for as long as the planetarium app is open. */
                pid_t pid = fork();
                if (pid == 0) {
                    close(sfd);
                    lx200_session(c);
                    close(c);
                    _exit(0);
                }
                close(c);
                if (pid > 0) { int st; waitpid(pid, &st, WNOHANG); }
            }
        }
        if (dfd >= 0 && FD_ISSET(dfd, &rf)) handle_discovery(dfd, port);
        {   /* Reap finished LX200 sessions -- but NOT the slew child, whose
             * liveness IS the Slewing property. Reaping it with waitpid(-1)
             * made every async slew report Slewing=false immediately. */
            int st; pid_t r;
            while ((r = waitpid(-1, &st, WNOHANG)) > 0)
                if (r == g_slew_pid) g_slew_pid = -1;
        }
    }
    return 0;
}
