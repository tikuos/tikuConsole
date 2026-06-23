/*
 * bridge.c - shared TikuOS host-bridge primitives (macOS).
 *
 * SLIP framing, a termios serial port, and a macOS utun device.  See bridge.h.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bridge.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <net/if_utun.h>
#include <netinet/in.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>

/* ------------------------------------------------------------------------- */
/* SLIP                                                                      */
/* ------------------------------------------------------------------------- */

size_t slip_encode(const uint8_t *pkt, size_t len, uint8_t *out)
{
    size_t o = 0;
    out[o++] = SLIP_END;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = pkt[i];
        if (b == SLIP_END) {
            out[o++] = SLIP_ESC;
            out[o++] = SLIP_ESC_END;
        } else if (b == SLIP_ESC) {
            out[o++] = SLIP_ESC;
            out[o++] = SLIP_ESC_ESC;
        } else if (b == 0x00) {
            out[o++] = SLIP_ESC;
            out[o++] = SLIP_ESC_NUL;
        } else {
            out[o++] = b;
        }
    }
    out[o++] = SLIP_END;
    return o;
}

size_t slip_unescape(const uint8_t *data, size_t len, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == SLIP_ESC && i + 1 < len) {
            uint8_t n = data[++i];
            out[o++] = (n == SLIP_ESC_END) ? SLIP_END :
                       (n == SLIP_ESC_ESC) ? SLIP_ESC :
                       (n == SLIP_ESC_NUL) ? 0x00 : n;
        } else {
            out[o++] = b;
        }
    }
    return o;
}

/* ------------------------------------------------------------------------- */
/* Serial (termios)                                                          */
/* ------------------------------------------------------------------------- */

int serial_open(const char *path, unsigned baud)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&t);
    t.c_cflag |= (CLOCAL | CREAD);   /* ignore modem lines, enable receiver */
    t.c_cflag &= ~CRTSCTS;           /* no hardware flow control */
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    /* macOS speed_t is the literal baud value, so 115200 works directly. */
    cfsetispeed(&t, (speed_t)baud);
    cfsetospeed(&t, (speed_t)baud);
    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------------- */
/* utun (macOS TUN)                                                          */
/* ------------------------------------------------------------------------- */

int utun_open(char *name, size_t name_len)
{
    int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) {
        return -1;
    }

    struct ctl_info ci;
    memset(&ci, 0, sizeof(ci));
    strlcpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof(ci.ctl_name));
    if (ioctl(fd, CTLIOCGINFO, &ci) < 0) {
        close(fd);
        return -1;
    }

    for (unsigned unit = 0; unit < 16; unit++) {
        struct sockaddr_ctl sc;
        memset(&sc, 0, sizeof(sc));
        sc.sc_len = sizeof(sc);
        sc.sc_family = AF_SYSTEM;
        sc.ss_sysaddr = AF_SYS_CONTROL;
        sc.sc_id = ci.ctl_id;
        sc.sc_unit = unit + 1;       /* sc_unit N+1 => interface utunN */
        if (connect(fd, (struct sockaddr *)&sc, sizeof(sc)) == 0) {
            socklen_t nl = (socklen_t)name_len;
            if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME,
                           name, &nl) < 0) {
                close(fd);
                return -1;
            }
            return fd;
        }
    }
    close(fd);
    return -1;
}

void utun_write(int fd, const uint8_t *pkt, size_t len)
{
    uint32_t af = htonl(AF_INET);
    struct iovec iov[2];
    iov[0].iov_base = &af;
    iov[0].iov_len = sizeof(af);
    iov[1].iov_base = (void *)pkt;
    iov[1].iov_len = len;
    (void)writev(fd, iov, 2);
}

/* ------------------------------------------------------------------------- */
/* Host helpers                                                              */
/* ------------------------------------------------------------------------- */

int bridge_run(const char *fmt, ...)
{
    char cmd[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    fprintf(stderr, "tikuconsole: + %s\r\n", cmd);
    return system(cmd);
}

int bridge_default_port(char *buf, size_t len)
{
    FILE *p = popen("ls /dev/cu.usbmodem* 2>/dev/null | head -1", "r");
    int ok = 0;
    if (p) {
        if (fgets(buf, (int)len, p)) {
            buf[strcspn(buf, "\r\n")] = 0;
            ok = (buf[0] != 0);
        }
        pclose(p);
    }
    return ok;
}
