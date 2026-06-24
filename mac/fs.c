/*
 * fs.c - tikuOS /data file access over the shell console (see fs.h).
 *
 * Synchronous request/response on a raw O_NONBLOCK serial fd, driven with
 * select() timeouts.  ls/df/rm parse text replies; get/put ride the device's
 * length-prefixed send/recv commands so files round-trip byte-exact.  This is
 * the C twin of the host tool tikuConsole/tikufs.py.
 *
 * Authors: WEISER Research Group, National University of Singapore
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <termios.h>

/* ------------------------------------------------------------------------- */
/* low-level helpers                                                         */
/* ------------------------------------------------------------------------- */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Wait up to wait_ms for fd to become readable.  1 = readable, 0 = timeout. */
static int wait_readable(int fd, int wait_ms)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    struct timeval tv;
    tv.tv_sec  = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    int r = select(fd + 1, &rd, NULL, NULL, &tv);
    return r > 0 && FD_ISSET(fd, &rd);
}

/* Binary-safe substring search.  Returns offset in hay, or -1. */
static long mem_find(const uint8_t *hay, size_t hlen,
                     const char *needle, size_t nlen)
{
    if (nlen == 0 || hlen < nlen) {
        return -1;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* Read until the line goes quiet for idle_ms (after at least one byte) or
 * total_ms elapses.  NUL-terminates.  Returns bytes read. */
static size_t read_idle(int fd, uint8_t *buf, size_t bufsz,
                        int idle_ms, int total_ms)
{
    size_t n = 0;
    long   t0 = now_ms(), last = t0;
    while (n < bufsz - 1) {
        long now = now_ms();
        if (now - t0 >= total_ms) {
            break;
        }
        if (n > 0 && now - last >= idle_ms) {
            break;
        }
        if (wait_readable(fd, 30)) {
            ssize_t r = read(fd, buf + n, bufsz - 1 - n);
            if (r > 0) {
                n += (size_t)r;
                last = now_ms();
            }
        }
    }
    buf[n] = 0;
    return n;
}

/* Remove ANSI/CSI escape sequences (the shell colours its prompt) in place. */
static void strip_ansi(char *s)
{
    char *d = s, *p = s;
    while (*p) {
        if (p[0] == 0x1b && p[1] == '[') {
            p += 2;
            while (*p && (*p < 0x40 || *p > 0x7e)) {   /* params / intermediates */
                p++;
            }
            if (*p) {
                p++;                                    /* final byte */
            }
        } else {
            *d++ = *p++;
        }
    }
    *d = 0;
}

/* Strip the echoed command line and the trailing shell prompt from a text
 * reply (in place). */
static void strip_reply(char *s, const char *cmd)
{
    strip_ansi(s);

    /* drop carriage returns */
    char *d = s, *p = s;
    for (; *p; p++) {
        if (*p != '\r') {
            *d++ = *p;
        }
    }
    *d = 0;

    /* trim trailing whitespace first, so the prompt line ends at '>' */
    size_t L = strlen(s);
    while (L > 0 && (s[L - 1] == '\n' || s[L - 1] == ' ' || s[L - 1] == '\t')) {
        s[--L] = 0;
    }

    /* drop the echoed command (first line) if it matches */
    size_t cl = strlen(cmd);
    if (cl && strncmp(s, cmd, cl) == 0 && (s[cl] == '\n' || s[cl] == 0)) {
        char *nl = strchr(s, '\n');
        if (nl) {
            memmove(s, nl + 1, strlen(nl + 1) + 1);
        } else {
            s[0] = 0;
        }
    }

    /* drop a trailing prompt line (ends with '>') */
    char  *lastnl = strrchr(s, '\n');
    char  *tail   = lastnl ? lastnl + 1 : s;
    size_t tl     = strlen(tail);
    if (tl > 0 && tail[tl - 1] == '>') {
        if (lastnl) {
            *lastnl = 0;
        } else {
            s[0] = 0;
        }
    }

    /* final trim */
    L = strlen(s);
    while (L > 0 && (s[L - 1] == '\n' || s[L - 1] == ' ' || s[L - 1] == '\t')) {
        s[--L] = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* text commands                                                             */
/* ------------------------------------------------------------------------- */

int fs_shell(int fd, const char *line, char *out, size_t outsz)
{
    char cmd[320];
    int  cn = snprintf(cmd, sizeof(cmd), "%s\r", line);
    tcflush(fd, TCIFLUSH);
    if (write(fd, cmd, (size_t)cn) < 0) {
        out[0] = 0;
        return 0;
    }
    size_t n = read_idle(fd, (uint8_t *)out, outsz, 250, 1500);
    out[n < outsz ? n : outsz - 1] = 0;
    strip_reply(out, line);
    return (int)strlen(out);
}

int fs_ls(int fd, char *out, size_t outsz)
{
    return fs_shell(fd, "ls /data", out, outsz);
}

int fs_ls_path(int fd, const char *abspath, char *out, size_t outsz)
{
    char line[300];
    snprintf(line, sizeof(line), "ls %s", abspath);
    return fs_shell(fd, line, out, outsz);
}

int fs_mkdir(int fd, const char *name, char *out, size_t outsz)
{
    char line[300];
    snprintf(line, sizeof(line), "mkdir /data/%s", name);
    return fs_shell(fd, line, out, outsz);
}

int fs_df(int fd, char *out, size_t outsz)
{
    return fs_shell(fd, "df", out, outsz);
}

int fs_rm(int fd, const char *name, char *out, size_t outsz)
{
    char line[300];
    snprintf(line, sizeof(line), "rm /data/%s", name);
    return fs_shell(fd, line, out, outsz);
}

int fs_parse_names(char *ls_text, const char *names[], int max)
{
    int   count = 0;
    char *save  = NULL;
    for (char *ln = strtok_r(ls_text, "\n", &save);
         ln && count < max;
         ln = strtok_r(NULL, "\n", &save)) {
        /* skip error lines ("ls: ...", "cannot ...") */
        if (strstr(ln, "cannot") || strncmp(ln, "ls:", 3) == 0) {
            continue;
        }
        /* take the last whitespace-delimited token as the filename */
        const char *name = NULL;
        char       *t, *s2 = NULL;
        for (t = strtok_r(ln, " \t", &s2); t; t = strtok_r(NULL, " \t", &s2)) {
            name = t;
        }
        if (name && name[0]) {
            names[count++] = name;
        }
    }
    return count;
}

/* ------------------------------------------------------------------------- */
/* binary transfer                                                           */
/* ------------------------------------------------------------------------- */

static void set_err(char *err, size_t esz, const char *msg)
{
    if (err && esz) {
        snprintf(err, esz, "%s", msg);
    }
}

/* Copy a device "recv:"/"send:" status line out of a buffer into err[]. */
static void set_err_line(char *err, size_t esz, const uint8_t *buf, size_t n,
                         const char *tag)
{
    long pos = mem_find(buf, n, tag, strlen(tag));
    if (pos < 0 || !err || !esz) {
        set_err(err, esz, "transfer failed");
        return;
    }
    size_t i = 0;
    const char *q = (const char *)buf + pos;
    while (*q && *q != '\n' && *q != '\r' && i < esz - 1) {
        err[i++] = *q++;
    }
    err[i] = 0;
}

long fs_get(int fd, const char *name, uint8_t *buf, size_t bufsz,
            char *err, size_t esz)
{
    char cmd[300];
    int  cn = snprintf(cmd, sizeof(cmd), "send /data/%s\r", name);
    tcflush(fd, TCIFLUSH);
    if (write(fd, cmd, (size_t)cn) < 0) {
        set_err(err, esz, "write error");
        return -1;
    }

    /* Accumulate until we have a complete "send: N\n" header (or an error). */
    uint8_t hdr[600];
    size_t  hn = 0;
    long    N = -1, t0 = now_ms();
    size_t  content_off = 0;
    while (now_ms() - t0 < 3000) {
        if (wait_readable(fd, 60)) {
            ssize_t r = read(fd, hdr + hn, sizeof(hdr) - 1 - hn);
            if (r > 0) {
                hn += (size_t)r;
                hdr[hn] = 0;
            }
        }
        long pos = mem_find(hdr, hn, "send:", 5);
        if (pos < 0) {
            if (hn >= sizeof(hdr) - 1) {
                break;
            }
            continue;
        }
        long nl = mem_find(hdr + pos, hn - (size_t)pos, "\n", 1);
        if (nl < 0) {
            if (hn >= sizeof(hdr) - 1) {
                break;
            }
            continue;                       /* header line not complete yet */
        }
        const char *p = (const char *)hdr + pos + 5;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p < '0' || *p > '9') {          /* "send: cannot read ..." */
            set_err_line(err, esz, hdr, hn, "send:");
            return -1;
        }
        N = 0;
        while (*p >= '0' && *p <= '9') {
            N = N * 10 + (*p++ - '0');
        }
        content_off = (size_t)pos + (size_t)nl + 1;
        break;
    }
    if (N < 0) {
        set_err(err, esz, "no send header (need a newer device build?)");
        return -1;
    }
    if ((size_t)N > bufsz) {
        set_err(err, esz, "file larger than buffer");
        return -1;
    }

    /* bytes already past the header newline are the first content bytes */
    size_t have = 0;
    if (hn > content_off) {
        have = hn - content_off;
        if (have > (size_t)N) {
            have = (size_t)N;
        }
        memcpy(buf, hdr + content_off, have);
    }
    t0 = now_ms();
    while (have < (size_t)N && now_ms() - t0 < 4000) {
        if (wait_readable(fd, 60)) {
            ssize_t r = read(fd, buf + have, (size_t)N - have);
            if (r > 0) {
                have += (size_t)r;
                t0 = now_ms();
            }
        }
    }
    if (have < (size_t)N) {
        set_err(err, esz, "short read");
        return -1;
    }
    /* swallow the trailing prompt so the console stays clean on watch resume */
    uint8_t scratch[64];
    read_idle(fd, scratch, sizeof(scratch), 120, 300);
    return N;
}

int fs_put(int fd, const char *name, const uint8_t *data, size_t len,
           char *err, size_t esz)
{
    if (len == 0) {                          /* recv needs N>=1; create empty */
        char out[200], line[300];
        snprintf(line, sizeof(line), "touch /data/%s", name);
        fs_shell(fd, line, out, sizeof(out));
        if (strstr(out, "cannot")) {
            set_err(err, esz, out);
            return -1;
        }
        return 0;
    }
    if (len > FS_SLOT_MAX) {
        char m[64];
        snprintf(m, sizeof(m), "file > %d B (one slot)", FS_SLOT_MAX);
        set_err(err, esz, m);
        return -1;
    }

    char cmd[300];
    int  cn = snprintf(cmd, sizeof(cmd), "recv /data/%s %zu\r", name, len);
    tcflush(fd, TCIFLUSH);
    if (write(fd, cmd, (size_t)cn) < 0) {
        set_err(err, esz, "write error");
        return -1;
    }

    /* wait for "recv: ready N" (or a rejection) */
    uint8_t b[400];
    size_t  bn = 0;
    long    t0 = now_ms();
    int     ready = 0;
    while (now_ms() - t0 < 3000) {
        if (wait_readable(fd, 60)) {
            ssize_t r = read(fd, b + bn, sizeof(b) - 1 - bn);
            if (r > 0) {
                bn += (size_t)r;
                b[bn] = 0;
            }
        }
        if (mem_find(b, bn, "ready", 5) >= 0) {
            ready = 1;
            break;
        }
        if (mem_find(b, bn, "must be", 7) >= 0 ||
            mem_find(b, bn, "Usage", 5) >= 0) {
            break;
        }
        if (bn >= sizeof(b) - 1) {
            break;
        }
    }
    if (!ready) {
        set_err_line(err, esz, b, bn, "recv:");
        return -1;
    }

    usleep(150000);                          /* device is now in its read loop */

    /* stream the payload (fd is non-blocking, so honour EAGAIN) */
    size_t off = 0;
    t0 = now_ms();
    while (off < len && now_ms() - t0 < 6000) {
        ssize_t w = write(fd, data + off, len - off);
        if (w > 0) {
            off += (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(2000);
        } else {
            set_err(err, esz, "write error");
            return -1;
        }
    }
    if (off < len) {
        set_err(err, esz, "write timeout");
        return -1;
    }

    /* confirm "recv: N bytes" */
    bn = 0;
    t0 = now_ms();
    while (now_ms() - t0 < 6000) {
        if (wait_readable(fd, 60)) {
            ssize_t r = read(fd, b + bn, sizeof(b) - 1 - bn);
            if (r > 0) {
                bn += (size_t)r;
                b[bn] = 0;
            }
        }
        if (mem_find(b, bn, "bytes", 5) >= 0) {
            uint8_t scratch[64];
            read_idle(fd, scratch, sizeof(scratch), 120, 300);
            return 0;
        }
        if (mem_find(b, bn, "failed", 6) >= 0 ||
            mem_find(b, bn, "timeout", 7) >= 0) {
            break;
        }
        if (bn >= sizeof(b) - 1) {
            break;
        }
    }
    set_err_line(err, esz, b, bn, "recv:");
    return -1;
}

/* ------------------------------------------------------------------------- */
/* headless self-test:  cc -DFS_TEST fs.c bridge.c -o fstest && ./fstest PORT */
/* ------------------------------------------------------------------------- */

#ifdef FS_TEST
#include "bridge.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /dev/cu.usbmodemXXXX\n", argv[0]);
        return 2;
    }
    int fd = serial_open(argv[1], 115200);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    char out[8192], err[200];
    fs_shell(fd, "", out, sizeof(out));               /* wake / sync */

    printf("== df ==\n%s\n", (fs_df(fd, out, sizeof(out)), out));

    /* split the literal right after \x00 so the hex escape can't swallow the
     * following 'b' as another hex digit; embedded NUL + multi-line. */
    static const char payload[] = "10 PRINT \"HI\"\n20 GOTO 10\nthe quick\x00" "brown";
    size_t plen = sizeof(payload) - 1;                /* keep the embedded NUL */
    printf("== put guitest.bin (%zu B, multi-line + NUL) ==\n", plen);
    if (fs_put(fd, "guitest.bin", (const uint8_t *)payload, plen,
               err, sizeof(err)) == 0) {
        printf("put: ok\n");
    } else {
        printf("put: FAIL (%s)\n", err);
    }

    fs_ls(fd, out, sizeof(out));
    printf("== ls ==\n%s\n", out);
    char        lscopy[8192];
    snprintf(lscopy, sizeof(lscopy), "%s", out);
    const char *names[64];
    int         nn = fs_parse_names(lscopy, names, 64);
    printf("parsed %d names:", nn);
    for (int i = 0; i < nn; i++) {
        printf(" [%s]", names[i]);
    }
    printf("\n");

    uint8_t got[FS_SLOT_MAX];
    long    n = fs_get(fd, "guitest.bin", got, sizeof(got), err, sizeof(err));
    if (n < 0) {
        printf("get: FAIL (%s)\n", err);
    } else {
        int ok = (n == (long)plen && memcmp(got, payload, plen) == 0);
        printf("get: %ld bytes -> %s\n", n, ok ? "BYTE-EXACT MATCH" : "MISMATCH");
    }

    fs_rm(fd, "guitest.bin", out, sizeof(out));
    printf("== rm -> ==\n%s\n", out[0] ? out : "(ok)");

    close(fd);
    return 0;
}
#endif
