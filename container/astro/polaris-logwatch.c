/* ===========================================================================
 * polaris-logwatch -- reliably follow /app/Mlog.txt.
 *
 * WHY THIS EXISTS. The app truncates Mlog.txt every few seconds (observed
 * 13439 -> 979 bytes in 20 s). Two naive approaches both lose data:
 *   - `tail -f`  follows by DESCRIPTOR, so after the first truncation it is
 *     reading a file nobody writes to any more and goes silent forever.
 *   - a 1-second shell poll loses anything written AND truncated inside one
 *     tick, which is exactly what happened to the 530 step:2 confirm message.
 *
 * This polls at 20 ms, follows by SIZE, and resets on shrink. That matters
 * beyond debugging: the auto-align daemon detects the alignment-arm message
 * (530 step:1) from this log, and a missed trigger means no auto-solve.
 *
 *   polaris-logwatch [--file F] [--match RE] [--out F] [--exec CMD] [--once]
 *
 * --match is a plain substring (several allowed, OR'd). --exec runs CMD with
 * the matching line in $POLARIS_LINE; with --once it exits after the first.
 *
 * MIT.
 * =========================================================================== */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#define MAXMATCH 16
#define CHUNK    65536

static const char *g_match[MAXMATCH];
static int         g_nmatch = 0;

/* strip ANSI colour so downstream greps and JSON stay clean */
static void strip_ansi(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == 0x1b && r[1] == '[') {
            r += 2;
            while (*r && *r != 'm') r++;
            if (*r) r++;
        } else *w++ = *r++;
    }
    *w = 0;
}

static int matches(const char *line) {
    int i;
    if (!g_nmatch) return 1;
    for (i = 0; i < g_nmatch; i++)
        if (strstr(line, g_match[i])) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *file = "/app/Mlog.txt", *outp = NULL, *cmd = NULL;
    int once = 0, i;
    long long last = -1;                 /* -1 = start at current EOF */
    FILE *out = NULL;
    char *buf = malloc(CHUNK + 1);
    char carry[8192]; size_t carrylen = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--file")  && i+1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--out")   && i+1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "--exec")  && i+1 < argc) cmd  = argv[++i];
        else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--from-start")) last = 0;
        else if (!strcmp(argv[i], "--match") && i+1 < argc) {
            if (g_nmatch < MAXMATCH) g_match[g_nmatch++] = argv[++i];
        } else {
            fprintf(stderr,
              "usage: %s [--file F] [--match SUBSTR]... [--out F] [--exec CMD] [--once] [--from-start]\n",
              argv[0]);
            return 2;
        }
    }
    if (!buf) return 1;
    signal(SIGPIPE, SIG_IGN);

    if (outp) {
        out = fopen(outp, "a");
        if (!out) { perror("open --out"); return 1; }
        setvbuf(out, NULL, _IOLBF, 0);
    }
    carry[0] = 0;

    for (;;) {
        struct stat st;
        int fd;
        if (stat(file, &st) != 0) { usleep(200000); continue; }

        if (last < 0) last = st.st_size;                 /* first sight: tail */
        if (st.st_size < last) last = 0;                 /* truncated: rewind */

        if (st.st_size > last) {
            fd = open(file, O_RDONLY);
            if (fd < 0) { usleep(20000); continue; }
            if (lseek(fd, (off_t)last, SEEK_SET) == (off_t)-1) { close(fd); last = 0; continue; }
            for (;;) {
                ssize_t n = read(fd, buf, CHUNK);
                char *p, *nl;
                if (n <= 0) break;
                buf[n] = 0;
                last += n;
                p = buf;
                while ((nl = strchr(p, '\n')) != NULL) {
                    char line[8192];
                    size_t len;
                    *nl = 0;
                    len = carrylen + strlen(p);
                    if (len < sizeof line) {
                        snprintf(line, sizeof line, "%s%s", carry, p);
                        carry[0] = 0; carrylen = 0;
                        strip_ansi(line);
                        if (line[0] && matches(line)) {
                            if (out) { fprintf(out, "%s\n", line); }
                            else     { printf("%s\n", line); fflush(stdout); }
                            if (cmd) {
                                setenv("POLARIS_LINE", line, 1);
                                if (system(cmd) == -1)
                                    fprintf(stderr, "exec failed: %s\n", strerror(errno));
                            }
                            if (once) { if (out) fclose(out); close(fd); return 0; }
                        }
                    } else { carry[0] = 0; carrylen = 0; }
                    p = nl + 1;
                }
                /* keep the partial tail for the next read */
                carrylen = strlen(p);
                if (carrylen >= sizeof carry) carrylen = 0, carry[0] = 0;
                else memcpy(carry, p, carrylen + 1);
            }
            close(fd);
        }
        usleep(20000);                                    /* 20 ms */
    }
}
