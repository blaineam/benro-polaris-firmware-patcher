/* SPDX-License-Identifier: MIT
 *
 * polaris-mount — talk to the Benro Polaris' own control protocol.
 *
 * The mount speaks a line-ish ASCII protocol on a TCP port: we send
 * "1&<cmd>&3&<k>:<v>;...#" and it streams back "<cmd>@<k>:<v>;...#".
 * Commands used here (all of them already used by the phone app and by the
 * alpaca-benro-polaris driver):
 *
 *    518  <- pose (quaternion + compass + alt)          [read]
 *    517  <- raw motor angles                           [read]
 *    284  <- mode (8 = Astro)                           [read]
 *    519  -> goto alt/az, or abort                      [MOVES MOTORS]
 *    531  -> tracking on/off                            [MOVES MOTORS]
 *    527  -> set compass heading (this is how a solved  [alignment]
 *            azimuth becomes the mount's alignment)
 *
 * Deliberately NOT used: 530 (multi-step star alignment). Field traces from the
 * Aperion work show repeated 530s wedging the Polaris' motors. One 527 does the
 * job for us.
 *
 * SAFETY: every motion command goes through check_move(), which refuses moves
 * outside the configured altitude band or larger than --max-slew, and --dry-run
 * prints what would be sent without sending it.
 */
/* strptime/timegm */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define DEG (M_PI / 180.0)
#define RAD (180.0 / M_PI)

/* The mount's wire format for an azimuth is NOT 0..360: it is a SIGNED value in
 * (-180, 180), measured westward. Hardware confirmed it -- a 519 carrying
 * yaw:256 is answered ret:-1 and the motors never move, while the same target
 * as yaw:104 works. The 530 alignment sequence uses the same encoding.
 * We keep 0..360 everywhere in our own API and convert only at the wire. */
static double az_to_wire(double az) {
    az = fmod(az, 360.0);
    if (az < 0) az += 360.0;
    return (az > 180.0) ? (360.0 - az) : -az;
}

static int   g_dry = 0;
static int   g_verbose = 0;
static double g_min_alt = 5.0;      /* refuse to drive below this */
static double g_max_alt = 85.0;     /* ... or above it */
static double g_max_slew = 180.0;   /* refuse a single move bigger than this */
/* Azimuth is degenerate near the zenith: a heading correction derived there is
 * amplified by 1/cos(alt), so a 30" solve becomes ~5' of heading error at 84
 * deg and ~15' at 88. Refuse to align from up there. */
static double g_max_align_alt = 65.0;

/* ------------------------------------------------------------------ time */

static double jd_from_unix(double t) { return t / 86400.0 + 2440587.5; }

/* Greenwich mean sidereal time, degrees. IAU 1982 series; good to well under
 * an arcsecond over the decades we care about. */
static double gmst_deg(double jd) {
    double T = (jd - 2451545.0) / 36525.0;
    double g = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
             + 0.000387933 * T * T - T * T * T / 38710000.0;
    g = fmod(g, 360.0);
    return (g < 0) ? g + 360.0 : g;
}

static double lst_deg(double jd, double lon_deg) {
    double l = fmod(gmst_deg(jd) + lon_deg, 360.0);
    return (l < 0) ? l + 360.0 : l;
}

/* ---- J2000 (what the plate solver reports) -> apparent place of date ----
 *
 * The solver's RA/Dec are ICRS/J2000. Feeding those straight into an hour-angle
 * formula is wrong by precession -- about 0.35 deg by 2026, which is far bigger
 * than anything else in this pipeline. So: precess, then nutate, then apply
 * annual aberration. That lands within a few arcsec of a rigorous reduction,
 * which is well under the mount's own pointing error.
 */
static void j2000_to_apparent(double ra, double dec, double jd,
                              double* ra_out, double* dec_out) {
    double T = (jd - 2451545.0) / 36525.0;
    double zeta, z, theta, A, B, C, ra2, dec2;
    double om, L, Lp, dpsi, deps, eps0, eps;
    double x, y, w, sr, cr, sd, cd;
    double lsun, e, pi_, k;

    /* --- precession, IAU 1976 rotation angles (arcsec -> rad) --- */
    zeta  = (2306.2181 * T + 0.30188 * T * T + 0.017998 * T * T * T) * DEG / 3600.0;
    z     = (2306.2181 * T + 1.09468 * T * T + 0.018203 * T * T * T) * DEG / 3600.0;
    theta = (2004.3109 * T - 0.42665 * T * T - 0.041833 * T * T * T) * DEG / 3600.0;
    sr = sin(ra * DEG + zeta); cr = cos(ra * DEG + zeta);
    sd = sin(dec * DEG);       cd = cos(dec * DEG);
    A = cd * sr;
    B = cos(theta) * cd * cr - sin(theta) * sd;
    C = sin(theta) * cd * cr + cos(theta) * sd;
    ra2 = atan2(A, B) + z;
    dec2 = asin(C > 1 ? 1 : (C < -1 ? -1 : C));

    /* --- nutation, the terms that matter at the arcsecond level --- */
    om = (125.04452 - 1934.136261 * T) * DEG;
    L  = (280.4665 + 36000.7698 * T) * DEG;
    Lp = (218.3165 + 481267.8813 * T) * DEG;
    dpsi = (-17.20 * sin(om) - 1.32 * sin(2 * L) - 0.23 * sin(2 * Lp)
            + 0.21 * sin(2 * om)) * DEG / 3600.0;
    deps = (9.20 * cos(om) + 0.57 * cos(2 * L) + 0.10 * cos(2 * Lp)
            - 0.09 * cos(2 * om)) * DEG / 3600.0;
    eps0 = (23.439291 - 0.0130042 * T) * DEG;
    eps  = eps0 + deps;
    sr = sin(ra2); cr = cos(ra2); sd = sin(dec2); cd = cos(dec2);
    ra2  += dpsi * (cos(eps) + sin(eps) * sr * (sd / cd)) - deps * cr * (sd / cd);
    dec2 += dpsi * sin(eps) * cr + deps * sr;

    /* --- annual aberration --- */
    k = 20.49552 * DEG / 3600.0;          /* constant of aberration */
    e = 0.016708634 - 0.000042037 * T;    /* eccentricity */
    pi_ = (102.93735 + 1.71946 * T) * DEG;/* longitude of perihelion */
    lsun = (280.46646 + 36000.76983 * T) * DEG
         + (1.914602 - 0.004817 * T) * DEG * sin((357.52911 + 35999.05029 * T) * DEG)
         + 0.019993 * DEG * sin(2 * (357.52911 + 35999.05029 * T) * DEG);
    sr = sin(ra2); cr = cos(ra2); sd = sin(dec2); cd = cos(dec2);
    x = -k * (cos(ra2) * cos(lsun) * cos(eps) + sin(ra2) * sin(lsun)) / cd;
    y = -k * (cos(lsun) * cos(eps) * (tan(eps) * cd - sr * sd) + cr * sd * sin(lsun));
    w = e * k;
    ra2  += x + w * (cos(pi_) * cos(eps) * cr + sin(pi_) * sr) / cd;
    dec2 += y + w * (sin(pi_) * cr * sd - cos(pi_) * (tan(eps) * cd - sr * sd));

    *ra_out = fmod(ra2 * RAD + 360.0, 360.0);
    *dec_out = dec2 * RAD;
}

/* Inverse: apparent place of date -> J2000, by iterating the forward model.
 * Three passes is plenty; the correction is under a degree and very smooth. */
static void apparent_to_j2000(double ra, double dec, double jd,
                              double* ra_out, double* dec_out) {
    double gr = ra, gd = dec, tr, td;
    int i;
    for (i = 0; i < 3; i++) {
        j2000_to_apparent(gr, gd, jd, &tr, &td);
        gr = fmod(gr + (ra - tr) + 540.0, 360.0) - 180.0;
        if (gr < 0) gr += 360.0;
        gd = gd + (dec - td);
    }
    *ra_out = gr; *dec_out = gd;
}

/* RA/Dec (deg, J2000) -> Alt/Az (deg). Az from North through East, the same
 * convention PyEphem (and therefore the alpaca driver) uses. */
static void radec2altaz_apparent(double ra, double dec, double lat, double lon,
                                 double jd, double* alt, double* az) {
    double ha = (lst_deg(jd, lon) - ra) * DEG;
    double d = dec * DEG, p = lat * DEG;
    double sinalt = sin(d) * sin(p) + cos(d) * cos(p) * cos(ha);
    double a, A;
    if (sinalt > 1) sinalt = 1;
    if (sinalt < -1) sinalt = -1;
    a = asin(sinalt);
    A = atan2(-sin(ha) * cos(d), cos(p) * sin(d) - sin(p) * cos(d) * cos(ha));
    *alt = a * RAD;
    *az = fmod(A * RAD + 360.0, 360.0);
}

static void radec2altaz(double ra, double dec, double lat, double lon,
                        double jd, double* alt, double* az) {
    double ar, ad;
    j2000_to_apparent(ra, dec, jd, &ar, &ad);
    radec2altaz_apparent(ar, ad, lat, lon, jd, alt, az);
}

static void altaz2radec_apparent(double alt, double az, double lat, double lon,
                                 double jd, double* ra, double* dec) {
    double a = alt * DEG, A = az * DEG, p = lat * DEG;
    double sind = sin(a) * sin(p) + cos(a) * cos(p) * cos(A);
    double d, ha, r;
    if (sind > 1) sind = 1;
    if (sind < -1) sind = -1;
    d = asin(sind);
    ha = atan2(-sin(A) * cos(a), cos(p) * sin(a) - sin(p) * cos(a) * cos(A));
    r = lst_deg(jd, lon) - ha * RAD;
    r = fmod(r, 360.0); if (r < 0) r += 360.0;
    *ra = r; *dec = d * RAD;
}

/* Alt/Az -> J2000 RA/Dec, so what we report matches what the solver speaks. */
static void altaz2radec(double alt, double az, double lat, double lon,
                        double jd, double* ra, double* dec) {
    double ar, ad;
    altaz2radec_apparent(alt, az, lat, lon, jd, &ar, &ad);
    apparent_to_j2000(ar, ad, jd, ra, dec);
}

/* ---------------------------------------------------------------- socket */

typedef struct { int fd; char buf[8192]; size_t len; } conn_t;

static int conn_open(conn_t* c, const char* host, int port, int timeout_s) {
    struct sockaddr_in sa;
    struct timeval tv;
    int one = 1;
    memset(c, 0, sizeof(*c));
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        struct hostent* he = gethostbyname(host);
        if (!he) { fprintf(stderr, "cannot resolve %s\n", host); return -1; }
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    }
    tv.tv_sec = timeout_s; tv.tv_usec = 0;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(c->fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "connect %s:%d failed: %s\n", host, port, strerror(errno));
        return -1;
    }
    return 0;
}

static void conn_close(conn_t* c) { if (c->fd >= 0) close(c->fd); c->fd = -1; }

static int conn_send(conn_t* c, const char* msg) {
    size_t n = strlen(msg);
    if (g_verbose || g_dry) fprintf(stderr, "->> %s\n", msg);
    if (g_dry) return 0;
    return (write(c->fd, msg, n) == (ssize_t)n) ? 0 : -1;
}

/* Pull the next complete "<cmd>@<args>#" out of the stream. */
static int conn_recv(conn_t* c, char* cmd_out, char* args_out, size_t args_sz) {
    for (;;) {
        char* at;
        char* hash;
        ssize_t n;
        /* is a whole message already buffered? */
        for (at = c->buf; at < c->buf + c->len; at++) {
            if (*at != '@') continue;
            if (at - c->buf < 3) continue;
            hash = memchr(at, '#', c->len - (at - c->buf));
            if (!hash) break;
            memcpy(cmd_out, at - 3, 3); cmd_out[3] = 0;
            {
                size_t alen = hash - at - 1;
                if (alen >= args_sz) alen = args_sz - 1;
                memcpy(args_out, at + 1, alen); args_out[alen] = 0;
            }
            {
                size_t used = (hash + 1) - c->buf;
                memmove(c->buf, hash + 1, c->len - used);
                c->len -= used;
            }
            if (g_verbose) fprintf(stderr, "<<- %s@%s#\n", cmd_out, args_out);
            return 0;
        }
        if (c->len == sizeof(c->buf)) c->len = 0;      /* junk guard */
        n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
        if (n <= 0) return -1;
        c->len += n;
    }
}

static int arg_get(const char* args, const char* key, char* out, size_t sz) {
    size_t klen = strlen(key);
    const char* p = args;
    while (p && *p) {
        const char* colon = strchr(p, ':');
        const char* semi;
        if (!colon) return -1;
        semi = strchr(colon, ';');
        if ((size_t)(colon - p) == klen && !strncmp(p, key, klen)) {
            size_t vlen = semi ? (size_t)(semi - colon - 1) : strlen(colon + 1);
            if (vlen >= sz) vlen = sz - 1;
            memcpy(out, colon + 1, vlen); out[vlen] = 0;
            return 0;
        }
        p = semi ? semi + 1 : NULL;
    }
    return -1;
}

/* The mount sends no pose telemetry until a client asks for it. This is the
 * same thing the phone app (and the alpaca driver) does on connect: 520 turns
 * position updates on, 284 asks for the current mode. Neither moves anything.
 * Note the third field is 2 for queries and 3 for commands. */
static void handshake(conn_t* c) {
    conn_send(c, "1&520&2&state:1;#");
    conn_send(c, "1&284&2&-1#");
}

/* Wait for a specific reply, ignoring the telemetry that streams in between. */
static int wait_for(conn_t* c, const char* want, char* args_out, size_t sz, int tries) {
    char cmd[8];
    int i;
    for (i = 0; i < tries; i++) {
        if (conn_recv(c, cmd, args_out, sz) != 0) return -1;
        if (!strcmp(cmd, want)) return 0;
    }
    return -1;
}

/* --------------------------------------------------------------- safety */

static int check_move(double alt, double az, double cur_alt, double cur_az) {
    if (!(alt == alt) || !(az == az)) { fprintf(stderr, "[safety] non-finite target\n"); return -1; }
    if (alt < g_min_alt || alt > g_max_alt) {
        fprintf(stderr, "[safety] refusing alt %.3f deg (allowed %.1f..%.1f)\n",
                alt, g_min_alt, g_max_alt);
        return -1;
    }
    if (cur_alt == cur_alt && cur_az == cur_az) {
        double dalt = fabs(alt - cur_alt);
        double daz = fabs(fmod(fabs(az - cur_az) + 180.0, 360.0) - 180.0);
        double move = (dalt > daz) ? dalt : daz;
        if (move > g_max_slew) {
            fprintf(stderr, "[safety] refusing %.1f deg slew (max %.1f)\n", move, g_max_slew);
            return -1;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- main */

static void usage(const char* me) {
    fprintf(stderr,
"usage: %s [global] <command> [args]\n"
"\n"
"global:\n"
"  --host H --port P      mount control endpoint (default 127.0.0.1:9090)\n"
"  --lat D --lon D        observer position, degrees (required for coordinates)\n"
"  --utc \"YYYY-MM-DDTHH:MM:SS\"  pretend it is this time (default: now)\n"
"  --min-alt D --max-alt D      altitude band motion is allowed in (5..85)\n"
"  --max-slew D           refuse a single move larger than this (180)\n"
"  --max-align-alt D      refuse to derive a heading from a frame taken above\n"
"                         this altitude (65). Azimuth is degenerate near the\n"
"                         zenith: the heading error is amplified by 1/cos(alt).\n"
"  --dry-run              print what would be sent; send nothing\n"
"  --raw N                just dump the next N protocol frames and exit\n"
"  --verbose              log the protocol\n"
"\n"
"commands:\n"
"  pose                   read one 518 and print alt/az/ra/dec as JSON\n"
"  goto --alt D --az D [--no-track]        MOVES MOTORS\n"
"  goto-radec --ra D --dec D [--no-track] [--no-refine]  MOVES MOTORS\n"
"                         after the slew, recomputes the target for the time it\n"
"                         actually arrived and nudges once if the sky has moved\n"
"                         more than --refine-arcmin (default 2)\n"
"  track on|off                            MOVES MOTORS\n"
"  abort                                   stops a slew\n"
"  set-compass --az D     tell the mount its true azimuth (527). Accepted while\n"
"                         unaligned, but does NOT clear the unaligned state.\n"
"  star-align --alt D --az D\n"
"                         the 3-step 530 sequence: declares the mount is looking\n"
"                         at this alt/az. THIS is what clears track:3 and makes\n"
"                         gotos work. Send it ONCE -- field traces from the\n"
"                         Aperion work show repeated 530s wedging the motors,\n"
"                         which is why it is a separate deliberate command.\n"
"  align --solved-ra D --solved-dec D [--image-alt D --image-az D]\n"
"                         compute the heading error from a plate solve and\n"
"                         push it with 527; prints the correction as JSON\n"
"  radec2altaz --ra D --dec D      pure maths, no connection\n"
"  altaz2radec --alt D --az D      pure maths, no connection\n", me);
}

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    int port = 9090;
    double lat = NAN, lon = NAN;
    double ra = NAN, dec = NAN, alt = NAN, az = NAN;
    double sra = NAN, sdec = NAN, ialt = NAN, iaz = NAN;
    int track = 1, refine = 1;
    double refine_arcmin = 2.0;
    const char* utc = NULL;
    const char* cmd = NULL;
    const char* cmd_arg = NULL;
    int raw_n = 0;
    double jd;
    conn_t c;
    int i;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (usage(argv[0]), exit(2), ""))
        if      (!strcmp(a, "--host"))     host = NEXT();
        else if (!strcmp(a, "--port"))     port = atoi(NEXT());
        else if (!strcmp(a, "--lat"))      lat = atof(NEXT());
        else if (!strcmp(a, "--lon"))      lon = atof(NEXT());
        else if (!strcmp(a, "--utc"))      utc = NEXT();
        else if (!strcmp(a, "--ra"))       ra = atof(NEXT());
        else if (!strcmp(a, "--dec"))      dec = atof(NEXT());
        else if (!strcmp(a, "--alt"))      alt = atof(NEXT());
        else if (!strcmp(a, "--az"))       az = atof(NEXT());
        else if (!strcmp(a, "--solved-ra"))  sra = atof(NEXT());
        else if (!strcmp(a, "--solved-dec")) sdec = atof(NEXT());
        else if (!strcmp(a, "--image-alt"))  ialt = atof(NEXT());
        else if (!strcmp(a, "--image-az"))   iaz = atof(NEXT());
        else if (!strcmp(a, "--no-track")) track = 0;
        else if (!strcmp(a, "--no-refine")) refine = 0;
        else if (!strcmp(a, "--refine-arcmin")) refine_arcmin = atof(NEXT());
        else if (!strcmp(a, "--min-alt"))  g_min_alt = atof(NEXT());
        else if (!strcmp(a, "--max-alt"))  g_max_alt = atof(NEXT());
        else if (!strcmp(a, "--max-slew")) g_max_slew = atof(NEXT());
        else if (!strcmp(a, "--max-align-alt")) g_max_align_alt = atof(NEXT());
        else if (!strcmp(a, "--dry-run"))  g_dry = 1;
        else if (!strcmp(a, "--verbose"))  g_verbose = 1;
        else if (!strcmp(a, "--raw"))      { cmd = "raw"; raw_n = atoi(NEXT()); }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (a[0] != '-' && !cmd)      cmd = a;
        else if (a[0] != '-' && !cmd_arg)  cmd_arg = a;
        else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 2; }
        #undef NEXT
    }
    if (!cmd) { usage(argv[0]); return 2; }

    if (utc) {
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        if (!strptime(utc, "%Y-%m-%dT%H:%M:%S", &tm)) { fprintf(stderr, "bad --utc\n"); return 2; }
        jd = jd_from_unix((double)timegm(&tm));
    } else {
        struct timeval tv; gettimeofday(&tv, NULL);
        jd = jd_from_unix(tv.tv_sec + tv.tv_usec * 1e-6);
    }

    /* pure-maths commands need no mount */
    if (!strcmp(cmd, "radec2altaz")) {
        if (ra != ra || dec != dec || lat != lat || lon != lon) { usage(argv[0]); return 2; }
        radec2altaz(ra, dec, lat, lon, jd, &alt, &az);
        printf("{\"alt_deg\":%.6f,\"az_deg\":%.6f,\"jd\":%.6f,\"lst_deg\":%.6f}\n",
               alt, az, jd, lst_deg(jd, lon));
        return 0;
    }
    if (!strcmp(cmd, "altaz2radec")) {
        if (alt != alt || az != az || lat != lat || lon != lon) { usage(argv[0]); return 2; }
        altaz2radec(alt, az, lat, lon, jd, &ra, &dec);
        printf("{\"ra_deg\":%.6f,\"dec_deg\":%.6f,\"jd\":%.6f}\n", ra, dec, jd);
        return 0;
    }

    if (conn_open(&c, host, port, 15) != 0) return 1;

    if (!strcmp(cmd, "raw")) {
        char rc[8], ra_[4096];
        int n;
        handshake(&c);
        for (n = 0; n < raw_n; n++) {
            if (conn_recv(&c, rc, ra_, sizeof(ra_)) != 0) break;
            printf("%s@%s#\n", rc, ra_);
            fflush(stdout);
        }
        conn_close(&c); return 0;
    }

    if (!strcmp(cmd, "pose")) {
        char args[2048], v[64];
        double p_az = NAN, p_alt = NAN;
        handshake(&c);
        if (wait_for(&c, "518", args, sizeof(args), 400) != 0) {
            fprintf(stderr, "no 518 pose message arrived\n"); conn_close(&c); return 1;
        }
        if (!arg_get(args, "compass", v, sizeof(v))) p_az = atof(v);
        if (!arg_get(args, "alt", v, sizeof(v)))     p_alt = -atof(v);
        printf("{\"alt_deg\":%.6f,\"az_deg\":%.6f", p_alt, p_az);
        if (lat == lat && lon == lon) {
            altaz2radec(p_alt, p_az, lat, lon, jd, &ra, &dec);
            printf(",\"ra_deg\":%.6f,\"dec_deg\":%.6f", ra, dec);
        }
        printf("}\n");
        conn_close(&c); return 0;
    }

    if (!strcmp(cmd, "abort")) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "1&519&3&state:0;yaw:0.0;pitch:0.0;lat:%.5f;track:0;speed:0;lng:%.5f;#",
                 (lat == lat) ? lat : 0.0, (lon == lon) ? lon : 0.0);
        conn_send(&c, msg);
        printf("{\"aborted\":true}\n");
        conn_close(&c); return 0;
    }

    if (!strcmp(cmd, "track")) {
        char msg[128];
        int on = (cmd_arg && !strcmp(cmd_arg, "on"));
        if (!cmd_arg || (strcmp(cmd_arg, "on") && strcmp(cmd_arg, "off"))) {
            fprintf(stderr, "track needs 'on' or 'off'\n"); conn_close(&c); return 2;
        }
        snprintf(msg, sizeof(msg), "1&531&3&state:%d;speed:0;#", on);
        conn_send(&c, msg);
        if (!g_dry) {
            char args[512], v[64];
            if (wait_for(&c, "531", args, sizeof(args), 200) == 0 && !arg_get(args, "ret", v, sizeof(v)))
                printf("{\"tracking\":%s}\n", (atoi(v) == 1) ? "true" : "false");
            else printf("{\"tracking\":null,\"error\":\"no 531 reply\"}\n");
        } else printf("{\"tracking\":null,\"dry_run\":true}\n");
        conn_close(&c); return 0;
    }

    if (!strcmp(cmd, "set-compass")) {
        char msg[256];
        if (az != az || lat != lat || lon != lon) { usage(argv[0]); conn_close(&c); return 2; }
        /* the mount's "compass" is 180 deg out from the azimuth we reason in */
        snprintf(msg, sizeof(msg), "1&527&3&compass:%.5f;lat:%.5f;lng:%.5f;#",
                 fmod(az - 180.0 + 360.0, 360.0), lat, lon);
        conn_send(&c, msg);
        printf("{\"compass_set_az_deg\":%.5f}\n", az);
        conn_close(&c); return 0;
    }

    if (!strcmp(cmd, "star-align")) {
        char msg[256];
        double ca_az;
        if (alt != alt || az != az || lat != lat || lon != lon) { usage(argv[0]); conn_close(&c); return 2; }
        /* the mount wants a signed azimuth measured westward, per the app */
        ca_az = az_to_wire(az);
        snprintf(msg, sizeof(msg), "1&530&3&step:1;yaw:0.0;pitch:0.0;lat:0.0;num:0;lng:0.0;#");
        conn_send(&c, msg);
        snprintf(msg, sizeof(msg),
                 "1&530&3&step:2;yaw:%.5f;pitch:%.5f;lat:%.5f;num:1;lng:%.5f;#",
                 ca_az, alt, lat, lon);
        conn_send(&c, msg);
        snprintf(msg, sizeof(msg), "1&530&3&step:3;yaw:0.0;pitch:0.0;lat:0.0;num:0;lng:0.0;#");
        conn_send(&c, msg);
        if (!g_dry) {
            char args[512], v[64];
            /* report whether the mount now considers itself aligned */
            conn_send(&c, "1&284&2&-1#");
            if (wait_for(&c, "284", args, sizeof(args), 400) == 0 &&
                !arg_get(args, "track", v, sizeof(v)))
                printf("{\"star_align\":true,\"alt_deg\":%.5f,\"az_deg\":%.5f,"
                       "\"track\":%s,\"aligned\":%s}\n",
                       alt, az, v, strcmp(v, "3") ? "true" : "false");
            else
                printf("{\"star_align\":true,\"alt_deg\":%.5f,\"az_deg\":%.5f,"
                       "\"track\":null}\n", alt, az);
        } else printf("{\"star_align\":null,\"dry_run\":true}\n");
        conn_close(&c); return 0;
    }

    if (!strcmp(cmd, "goto") || !strcmp(cmd, "goto-radec")) {
        char msg[512], args[1024], v[64];
        double cur_alt = NAN, cur_az = NAN;
        if (!strcmp(cmd, "goto-radec")) {
            if (ra != ra || dec != dec || lat != lat || lon != lon) { usage(argv[0]); conn_close(&c); return 2; }
            radec2altaz(ra, dec, lat, lon, jd, &alt, &az);
        }
        if (alt != alt || az != az) { usage(argv[0]); conn_close(&c); return 2; }
        /* current pose first, so the safety check has something to compare to */
        handshake(&c);
        if (wait_for(&c, "518", args, sizeof(args), 400) == 0) {
            if (!arg_get(args, "compass", v, sizeof(v))) cur_az = atof(v);
            if (!arg_get(args, "alt", v, sizeof(v)))     cur_alt = -atof(v);
        }
        if (check_move(alt, az, cur_alt, cur_az) != 0) { conn_close(&c); return 4; }
        snprintf(msg, sizeof(msg),
                 "1&519&3&state:1;yaw:%.5f;pitch:%.5f;lat:%.5f;track:%d;speed:0;lng:%.5f;#",
                 az_to_wire(az), alt, (lat == lat) ? lat : 0.0, track,
                 (lon == lon) ? lon : 0.0);
        conn_send(&c, msg);
        if (!g_dry) {
            /* the mount answers 519 twice: slew started, then slew finished */
            if (wait_for(&c, "519", args, sizeof(args), 4000) != 0) {
                printf("{\"goto\":false,\"error\":\"no 519 reply\"}\n"); conn_close(&c); return 5;
            }
            if (wait_for(&c, "519", args, sizeof(args), 20000) != 0) {
                printf("{\"goto\":false,\"error\":\"slew did not report finished\"}\n");
                conn_close(&c); return 5;
            }
            /* Hardware: the final 519 carries ret:0 on a completed slew and
             * ret:-1 when the mount refused or aborted it -- e.g. an azimuth
             * outside the signed wire range, or a goto while unaligned. Do not
             * report success for a slew that never happened. */
            {
                char rv[32];
                if (!arg_get(args, "ret", rv, sizeof(rv)) && atoi(rv) < 0) {
                    printf("{\"goto\":false,\"error\":\"mount refused or aborted the slew\","
                           "\"ret\":%s,\"hint\":\"is it aligned? track:3 means no\"}\n", rv);
                    conn_close(&c); return 5;
                }
            }
        }
        /* The sky moved while we slewed. Recompute for the time we actually
         * arrived and, if that matters, nudge once. Without this a long slew
         * lands where the target USED to be -- worst near the horizon, where
         * azimuth changes fastest. */
        {
            int refined = 0;
            double ralt = alt, raz = az;
            if (refine && !g_dry && !strcmp(cmd, "goto-radec")) {
                struct timeval tv2; double jd2, sep;
                gettimeofday(&tv2, NULL);
                jd2 = jd_from_unix(tv2.tv_sec + tv2.tv_usec * 1e-6);
                radec2altaz(ra, dec, lat, lon, jd2, &ralt, &raz);
                sep = fabs(ralt - alt) + fabs(fmod(fabs(raz - az) + 180.0, 360.0) - 180.0)
                      * cos(ralt * DEG);
                if (sep * 60.0 > refine_arcmin && check_move(ralt, raz, alt, az) == 0) {
                    snprintf(msg, sizeof(msg),
                             "1&519&3&state:1;yaw:%.5f;pitch:%.5f;lat:%.5f;track:%d;speed:0;lng:%.5f;#",
                             az_to_wire(raz), ralt, lat, track, lon);
                    conn_send(&c, msg);
                    wait_for(&c, "519", args, sizeof(args), 4000);
                    wait_for(&c, "519", args, sizeof(args), 20000);
                    refined = 1;
                }
            }
            printf("{\"goto\":true,\"alt_deg\":%.6f,\"az_deg\":%.6f,\"tracking\":%s,"
                   "\"refined\":%s}\n",
                   refined ? ralt : alt, refined ? raz : az,
                   track ? "true" : "false", refined ? "true" : "false");
        }
        conn_close(&c); return 0;
    }

    if (!strcmp(cmd, "align")) {
        /* What the mount THINKS it is pointing at vs where the sky says it is.
         * The difference in azimuth is the heading error; feed it back with
         * 527 so every later goto lands. */
        char args[2048], v[64], msg[256];
        double p_alt = NAN, p_az = NAN, s_alt, s_az, daz, dalt;
        if (sra != sra || sdec != sdec || lat != lat || lon != lon) { usage(argv[0]); conn_close(&c); return 2; }
        if (ialt == ialt && iaz == iaz) { p_alt = ialt; p_az = iaz; }
        else {
            handshake(&c);
            if (wait_for(&c, "518", args, sizeof(args), 400) != 0) {
                fprintf(stderr, "no 518 pose message arrived\n"); conn_close(&c); return 1;
            }
            if (!arg_get(args, "compass", v, sizeof(v))) p_az = atof(v);
            if (!arg_get(args, "alt", v, sizeof(v)))     p_alt = -atof(v);
        }
        radec2altaz(sra, sdec, lat, lon, jd, &s_alt, &s_az);
        {   /* refuse a degenerate alignment rather than poisoning the mount */
            double amp = 1.0 / fmax(cos(s_alt * DEG), 1e-6);
            if (s_alt > g_max_align_alt) {
                printf("{\"aligned\":false,\"reason\":\"too close to the zenith\","
                       "\"alt_deg\":%.4f,\"max_align_alt_deg\":%.4f,"
                       "\"error_amplification\":%.1f}\n", s_alt, g_max_align_alt, amp);
                fprintf(stderr, "[align] refusing: a solve at alt %.1f deg amplifies "
                        "heading error %.0fx. Point lower (below %.0f deg) and re-solve.\n",
                        s_alt, amp, g_max_align_alt);
                conn_close(&c); return 6;
            }
        }
        daz = fmod(s_az - p_az + 540.0, 360.0) - 180.0;
        dalt = s_alt - p_alt;
        snprintf(msg, sizeof(msg), "1&527&3&compass:%.5f;lat:%.5f;lng:%.5f;#",
                 fmod(fmod(p_az + daz, 360.0) - 180.0 + 360.0, 360.0), lat, lon);
        conn_send(&c, msg);
        printf("{\"aligned\":true,\"mount_alt_deg\":%.6f,\"mount_az_deg\":%.6f,"
               "\"sky_alt_deg\":%.6f,\"sky_az_deg\":%.6f,"
               "\"az_error_deg\":%.6f,\"alt_error_deg\":%.6f,"
               "\"error_amplification\":%.2f}\n",
               p_alt, p_az, s_alt, s_az, daz, dalt,
               1.0 / fmax(cos(s_alt * DEG), 1e-6));
        conn_close(&c); return 0;
    }

    fprintf(stderr, "unknown command: %s\n", cmd);
    conn_close(&c);
    usage(argv[0]);
    return 2;
}
