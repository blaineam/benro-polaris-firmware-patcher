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
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define PORT_DEFAULT   8080
#define JOB_LOG        "/tmp/polaris-job.log"
#define JOB_STATUS     "/tmp/polaris-job.status"   /* idle|running|done|failed */
#define JOB_RESULT     "/tmp/polaris-job.result"   /* solver JSON */
#define ASTRO_DIR      "/app/astro"

static const char *g_astro = ASTRO_DIR;
static double g_lat = 0.0, g_lon = 0.0;
static int    g_have_pos = 0;
static double g_focal = 400.0;

/* ---------------------------------------------------------------- helpers */

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

    snprintf(cmd, sizeof cmd, "%s/polaris-mount --host 127.0.0.1 pose 2>/dev/null", g_astro);
    if (run_capture(cmd, buf, sizeof buf) >= 0 && buf[0]) {
        p = strstr(buf, "\"alt_deg\":"); if (p) { *alt = atof(p + 10); ok = 1; }
        p = strstr(buf, "\"az_deg\":");  if (p) { *az  = atof(p + 9);  ok = 1; }
    }
    snprintf(cmd, sizeof cmd, "%s/polaris-mount --host 127.0.0.1 state 2>/dev/null", g_astro);
    if (run_capture(cmd, buf, sizeof buf) >= 0 && raw)
        snprintf(raw, rawcap, "%s", buf);
    return ok;
}

/* ------------------------------------------------------------------- jobs */

/* Fork a solve job. Parent returns immediately so the UI can poll. */
static void start_solve(int apply) {
    pid_t pid;
    char status[32];
    read_file(JOB_STATUS, status, sizeof status);
    if (strncmp(status, "running", 7) == 0) return;   /* already busy */

    set_status("running");
    unlink(JOB_RESULT);

    pid = fork();
    if (pid < 0) { set_status("failed"); return; }
    if (pid > 0) { int st; waitpid(pid, &st, 0); return; }   /* reap the middle child */

    /* ---- middle child: fork again so the job is orphaned to init ---- */
    if (fork() > 0) _exit(0);

    /* ---- grandchild: the actual job ---- */
    setsid();
    {
        char cmd[1024];
        int fd = open(JOB_LOG, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        snprintf(cmd, sizeof cmd,
                 "LAT=%.6f LON=%.6f sh %s/solve-now.sh --focal %.0f %s > %s 2>>%s",
                 g_lat, g_lon, g_astro, g_focal,
                 apply ? "--apply" : "", JOB_RESULT, JOB_LOG);
        int rc = system(cmd);
        set_status(rc == 0 ? "done" : "failed");
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

static void respond_json(int fd, const char *json) {
    respond(fd, 200, "application/json", json, strlen(json));
}

/* query/body param lookup: name=value, returns 1 if found */
static int param(const char *qs, const char *name, char *out, size_t cap) {
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
        if (eq && (size_t)(eq - p) == nl && strncasecmp(p, name, nl) == 0) {
            size_t vl = (size_t)(amp - eq - 1);
            if (vl > cap - 1) vl = cap - 1;
            memcpy(out, eq + 1, vl); out[vl] = 0;
            return 1;
        }
        p = amp;
    }
    return 0;
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

static void alpaca_value(int fd, const char *qs, const char *value_json) {
    char cid[32], out[1024];
    long c = param(qs, "ClientTransactionID", cid, sizeof cid) ? atol(cid) : 0;
    snprintf(out, sizeof out,
             "{\"Value\":%s,\"ClientTransactionID\":%ld,\"ServerTransactionID\":%lu,"
             "\"ErrorNumber\":0,\"ErrorMessage\":\"\"}",
             value_json, c, ++g_server_txn);
    respond_json(fd, out);
}

static void alpaca_error(int fd, const char *qs, int num, const char *msg) {
    char cid[32], out[1024], esc[512];
    long c = param(qs, "ClientTransactionID", cid, sizeof cid) ? atol(cid) : 0;
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
    if (!strcmp(m, "canslew") || !strcmp(m, "canslewasync") || !strcmp(m, "cansync"))
        { alpaca_value(fd, qs, "true"); return 1; }
    if (!strcmp(m, "atpark") || !strcmp(m, "athome") || !strcmp(m, "ispulseguiding"))
        { alpaca_value(fd, qs, "false"); return 1; }
    if (!strcmp(m, "sideofpier")) { alpaca_value(fd, qs, "-1"); return 1; }

    if (!strcmp(m, "connected")) {
        if (!strcmp(method, "PUT")) { alpaca_value(fd, qs, "null"); return 1; }
        alpaca_value(fd, qs, "true"); return 1;
    }
    if (!strcmp(m, "sitelatitude"))  { char b[64]; snprintf(b,sizeof b,"%.6f",g_lat); alpaca_value(fd, qs, b); return 1; }
    if (!strcmp(m, "sitelongitude")) { char b[64]; snprintf(b,sizeof b,"%.6f",g_lon); alpaca_value(fd, qs, b); return 1; }

    /* --- where are we pointing? --- */
    if (!strcmp(m, "rightascension") || !strcmp(m, "declination")) {
        double ra = 0, dec = 0;
        char b[64];
        if (!last_solution(&ra, &dec)) {
            /* No solve yet: fall back to the mount's own idea via altaz->radec. */
            char cmd[512], out[512], raw[512];
            double alt = 0, az = 0;
            mount_pose(&alt, &az, raw, sizeof raw);
            snprintf(cmd, sizeof cmd,
                     "%s/polaris-mount --lat %.6f --lon %.6f altaz2radec --alt %.6f --az %.6f 2>/dev/null",
                     g_astro, g_lat, g_lon, alt, az);
            if (run_capture(cmd, out, sizeof out) == 0) {
                const char *p = strstr(out, "\"ra_deg\":");
                if (p) ra = atof(p + 9);
                p = strstr(out, "\"dec_deg\":");
                if (p) dec = atof(p + 10);
            }
        }
        if (!strcmp(m, "rightascension")) snprintf(b, sizeof b, "%.6f", ra / 15.0); /* HOURS */
        else                              snprintf(b, sizeof b, "%.6f", dec);
        alpaca_value(fd, qs, b);
        return 1;
    }
    if (!strcmp(m, "slewing")) {
        char st[32]; read_file(JOB_STATUS, st, sizeof st);
        alpaca_value(fd, qs, strncmp(st, "running", 7) == 0 ? "true" : "false");
        return 1;
    }
    if (!strcmp(m, "tracking")) { alpaca_value(fd, qs, "true"); return 1; }
    if (!strcmp(m, "utcdate")) {
        char b[64]; time_t t = time(NULL); struct tm g;
        gmtime_r(&t, &g);
        snprintf(b, sizeof b, "\"%04d-%02d-%02dT%02d:%02d:%02dZ\"",
                 g.tm_year+1900, g.tm_mon+1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
        alpaca_value(fd, qs, b); return 1;
    }

    /* --- slew / sync --- */
    if (!strcmp(m, "slewtocoordinates") || !strcmp(m, "slewtocoordinatesasync") ||
        !strcmp(m, "synctocoordinates")) {
        char ra_s[64], dec_s[64], cmd[512], out[1024];
        const char *src = (body && *body) ? body : qs;
        if (!param(src, "RightAscension", ra_s, sizeof ra_s) ||
            !param(src, "Declination", dec_s, sizeof dec_s)) {
            alpaca_error(fd, qs, 1025, "RightAscension/Declination required");
            return 1;
        }
        double ra_deg = atof(ra_s) * 15.0;            /* hours -> degrees */
        double dec_deg = atof(dec_s);
        if (!strcmp(m, "synctocoordinates"))
            snprintf(cmd, sizeof cmd,
                     "%s/polaris-mount --lat %.6f --lon %.6f align --solved-ra %.6f --solved-dec %.6f 2>&1",
                     g_astro, g_lat, g_lon, ra_deg, dec_deg);
        else
            snprintf(cmd, sizeof cmd,
                     "%s/polaris-mount --lat %.6f --lon %.6f goto-radec --ra %.6f --dec %.6f 2>&1",
                     g_astro, g_lat, g_lon, ra_deg, dec_deg);
        run_capture(cmd, out, sizeof out);
        alpaca_value(fd, qs, "null");
        return 1;
    }
    if (!strcmp(m, "abortslew")) {
        char cmd[256], out[512];
        snprintf(cmd, sizeof cmd, "%s/polaris-mount --host 127.0.0.1 abort 2>&1", g_astro);
        run_capture(cmd, out, sizeof out);
        alpaca_value(fd, qs, "null");
        return 1;
    }

    (void)v;
    alpaca_error(fd, qs, 1024, "not implemented");
    return 1;
}

/* --------------------------------------------------------------- web page */
static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Polaris Plate Solver</title><style>"
":root{--bg:#0b0e14;--fg:#e6e6e6;--dim:#8b93a7;--acc:#5aa9e6;--ok:#4caf50;--err:#e05252;--card:#151a23}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);"
"font:15px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;padding:16px}"
"h1{font-size:19px;margin:0 0 14px}h2{font-size:13px;text-transform:uppercase;"
"letter-spacing:.08em;color:var(--dim);margin:0 0 8px}"
".card{background:var(--card);border-radius:10px;padding:14px;margin-bottom:12px}"
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
"<h1>Benro Polaris &mdash; Plate Solver</h1>"
"<div class=card><h2>Solve</h2>"
"<button id=b1 onclick=go(0)>Capture &amp; Solve</button>"
"<button id=b2 class=alt onclick=go(1)>Solve &amp; Apply to Mount</button>"
"<div class=row style='margin-top:10px'><span>status</span>"
"<span><span id=st class='s idle'>idle</span></span></div></div>"
"<div class=card><h2>Last solution</h2><div id=sol></div></div>"
"<div class=card><h2>Mount</h2><div id=mnt></div></div>"
"<div class=card><h2>Log</h2><pre id=log>&hellip;</pre></div>"
"<script>\n"
"function f(n,d){return (n===undefined||n===null||isNaN(n))?'--':Number(n).toFixed(d)}\n"
"function hms(deg){if(deg==null||isNaN(deg))return '--';var h=deg/15,H=Math.floor(h),"
"m=(h-H)*60,M=Math.floor(m),S=(m-M)*60;return H+'h '+M+'m '+S.toFixed(1)+'s'}\n"
"function dms(deg){if(deg==null||isNaN(deg))return '--';var s=deg<0?'-':'+';deg=Math.abs(deg);"
"var D=Math.floor(deg),m=(deg-D)*60,M=Math.floor(m),S=(m-M)*60;"
"return s+D+'\\u00b0 '+M+'\\u2032 '+S.toFixed(1)+'\\u2033'}\n"
"function row(k,v){return '<div class=row><span>'+k+'</span><span>'+v+'</span></div>'}\n"
"function go(a){document.getElementById('b1').disabled=true;document.getElementById('b2').disabled=true;\n"
"  fetch('/api/solve'+(a?'?apply=1':''),{method:'POST'}).then(tick)}\n"
"function tick(){fetch('/api/state').then(r=>r.json()).then(s=>{\n"
"  var st=document.getElementById('st');st.textContent=s.status;st.className='s '+s.status;\n"
"  var busy=s.status==='running';\n"
"  document.getElementById('b1').disabled=busy;document.getElementById('b2').disabled=busy;\n"
"  var o=s.solution;\n"
"  document.getElementById('sol').innerHTML = (o&&o.solved)\n"
"    ? row('RA',hms(o.ra_deg))+row('Dec',dms(o.dec_deg))+row('roll',f(o.roll_deg,2)+'\\u00b0')\n"
"      +row('pixel scale',f(o.pixscale_arcsec,3)+'\\u2033/px')\n"
"      +row('field',f(o.field_w_deg,2)+'\\u00b0 \\u00d7 '+f(o.field_h_deg,2)+'\\u00b0')\n"
"      +row('matches',o.nmatch+' of '+o.nfield)+row('solve time',f(o.solve_seconds,2)+' s')\n"
"    : '<div class=row><span>no solution yet</span><span></span></div>';\n"
"  document.getElementById('mnt').innerHTML=row('alt',f(s.alt,3)+'\\u00b0')+row('az',f(s.az,3)+'\\u00b0')\n"
"    +row('aligned',s.aligned?'yes':'no')+row('tracking',s.tracking?'yes':'no');\n"
"  document.getElementById('log').textContent=s.log||'(empty)';\n"
"})}\n"
"tick();setInterval(tick,1500);\n"
"</script></body></html>";

/* ------------------------------------------------------------------- main */

static void handle(int fd) {
    char req[8192], *body = NULL;
    ssize_t n;
    char method[16] = {0}, target[1024] = {0}, path[1024], *qs;

    n = read(fd, req, sizeof req - 1);
    if (n <= 0) return;
    req[n] = 0;
    { char *e = strstr(req, "\r\n\r\n"); if (e) body = e + 4; }
    if (sscanf(req, "%15s %1023s", method, target) != 2) return;

    if (!strcmp(method, "OPTIONS")) { respond(fd, 200, "text/plain", "", 0); return; }

    snprintf(path, sizeof path, "%s", target);
    qs = strchr(path, '?');
    if (qs) { *qs = 0; qs++; } else qs = (char *)"";

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(fd, 200, "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
        return;
    }

    if (!strcmp(path, "/api/state")) {
        char status[32], log[4096], logesc[8192], result[4096], raw[1024], out[16384];
        double alt = 0, az = 0;
        int aligned = 0, tracking = 0;
        read_file(JOB_STATUS, status, sizeof status);
        if (!status[0]) snprintf(status, sizeof status, "idle");
        { char *e = status + strlen(status);
          while (e > status && (e[-1]=='\n'||e[-1]==' ')) *--e = 0; }
        read_tail(JOB_LOG, log, sizeof log);
        json_escape(log, logesc, sizeof logesc);
        read_file(JOB_RESULT, result, sizeof result);
        { char *e = strchr(result, '\n'); if (e) *e = 0; }
        if (!strstr(result, "\"solved\"")) snprintf(result, sizeof result, "null");
        if (mount_pose(&alt, &az, raw, sizeof raw)) {
            aligned  = strstr(raw, "\"aligned\":true") != NULL;
            { const char *t = strstr(raw, "\"track\":");
              tracking = t && atoi(t + 8) != 0; }
        }
        snprintf(out, sizeof out,
            "{\"status\":\"%s\",\"alt\":%.4f,\"az\":%.4f,\"aligned\":%s,\"tracking\":%s,"
            "\"solution\":%s,\"log\":\"%s\"}",
            status, alt, az, aligned?"true":"false", tracking?"true":"false",
            result, logesc);
        respond_json(fd, out);
        return;
    }

    if (!strcmp(path, "/api/solve")) {
        char a[16];
        int apply = param(qs, "apply", a, sizeof a) && a[0] == '1';
        start_solve(apply);
        respond_json(fd, "{\"started\":true}");
        return;
    }

    if (handle_alpaca(fd, method, path, qs, body)) return;

    respond(fd, 404, "text/plain", "not found\n", 10);
}

int main(int argc, char **argv) {
    int port = PORT_DEFAULT, sfd, one = 1, i;
    struct sockaddr_in a;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i+1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lat") && i+1 < argc) { g_lat = atof(argv[++i]); g_have_pos = 1; }
        else if (!strcmp(argv[i], "--lon") && i+1 < argc) { g_lon = atof(argv[++i]); g_have_pos = 1; }
        else if (!strcmp(argv[i], "--focal") && i+1 < argc) g_focal = atof(argv[++i]);
        else if (!strcmp(argv[i], "--astro") && i+1 < argc) g_astro = argv[++i];
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

    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(sfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(sfd, 16) < 0) { perror("listen"); return 1; }

    if (access(JOB_STATUS, F_OK) != 0) set_status("idle");
    fprintf(stderr, "polaris-httpd on :%d  (lat %.5f lon %.5f focal %.0fmm)\n",
            port, g_lat, g_lon, g_focal);

    for (;;) {
        int c = accept(sfd, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        handle(c);
        close(c);
    }
    return 0;
}
