/*
 * slmux.c - SLIP + console multiplexer for TikuOS, macOS edition (CLI).
 *
 * When the board is in `slip` mode it interleaves, on one USB-serial line, both
 * the interactive shell (ASCII text) and SLIP/IP frames (0xC0-delimited).
 * slmux demultiplexes that single wire into two things at once:
 *
 *   * a utun network interface -- so the macOS kernel's own networking rides
 *     it: `ping 172.16.7.2`, `curl http://172.16.7.2`, etc.
 *   * an interactive console -- the board's shell text on stdout, your
 *     keystrokes (stdin) sent back, so you can still type commands.
 *
 * One cable carries the shell AND real networking simultaneously.  The shared
 * SLIP / serial / utun primitives live in bridge.c; this file is just the
 * command-line front end (a select() loop + raw-tty console).  The GTK GUI
 * (tikuconsole) wraps the same bridge core.
 *
 * Needs root (creates a utun device and configures the interface):
 *
 *     sudo ./slmux /dev/cu.usbmodemXXXX
 *
 * Type `slip` once in the console to put the board in SLIP mode.  Quit: Ctrl-].
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bridge.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/select.h>

/* ------------------------------------------------------------------------- */
/* Console raw-mode (restored on exit)                                       */
/* ------------------------------------------------------------------------- */

static struct termios g_saved_tty;
static int g_tty_raw;

static void tty_restore(void)
{
    if (g_tty_raw) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &g_saved_tty);
        g_tty_raw = 0;
    }
}

static void tty_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &g_saved_tty) == 0) {
        struct termios t = g_saved_tty;
        cfmakeraw(&t);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
            g_tty_raw = 1;
            atexit(tty_restore);
        }
    }
}

static void die(const char *what)
{
    fprintf(stderr, "slmux: %s: %s\r\n", what, strerror(errno));
    exit(1);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [PORT] [--baud N] [--host-ip A] [--board-ip B]\n"
        "  PORT       serial device (default: first /dev/cu.usbmodem*)\n"
        "  --baud     baud rate (default 115200)\n"
        "  --host-ip  this Mac's address on the link (default 172.16.7.1)\n"
        "  --board-ip the board's address (default 172.16.7.2)\n"
        "\nRun as root (creates a utun device). Type 'slip' in the console to\n"
        "put the board in SLIP mode. Quit with Ctrl-].\n", argv0);
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *port = NULL;
    unsigned baud = 115200;
    const char *host_ip = "172.16.7.1";
    const char *board_ip = "172.16.7.2";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            baud = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--host-ip") == 0 && i + 1 < argc) {
            host_ip = argv[++i];
        } else if (strcmp(argv[i], "--board-ip") == 0 && i + 1 < argc) {
            board_ip = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            port = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    char auto_port[256] = {0};
    if (port == NULL) {
        if (bridge_default_port(auto_port, sizeof(auto_port))) {
            port = auto_port;
        } else {
            fprintf(stderr, "slmux: no /dev/cu.usbmodem* found; pass PORT.\n");
            return 1;
        }
    }

    if (geteuid() != 0) {
        fprintf(stderr, "slmux: must run as root (creates a utun device). "
                        "Use sudo.\n");
        return 1;
    }

    int ser = serial_open(port, baud);
    if (ser < 0) {
        die("open serial");
    }

    char utun_name[32] = {0};
    int utun = utun_open(utun_name, sizeof(utun_name));
    if (utun < 0) {
        die("create utun");
    }

    /* Point-to-point link: local = this Mac, peer = the board. */
    bridge_run("ifconfig %s inet %s %s up", utun_name, host_ip, board_ip);
    bridge_run("route -q -n add -net 172.16.7.0/24 -interface %s 2>/dev/null",
               utun_name);

    fprintf(stderr,
        "slmux: %s <-> %s  (board %s, host %s)\r\n"
        "slmux: type 'slip' to enable the board's SLIP mode. Quit: Ctrl-]\r\n",
        port, utun_name, board_ip, host_ip);

    tty_raw();

    /* Board->host SLIP reassembly state. */
    int in_frame = 0;
    int slip_armed = 0;     /* self-syncing: a frame starts only at END + 0x4N */
    uint8_t frame[BRIDGE_MTU * 2];
    size_t flen = 0;

    int maxfd = ser;
    if (utun > maxfd) maxfd = utun;
    if (STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ser, &rfds);
        FD_SET(utun, &rfds);
        FD_SET(STDIN_FILENO, &rfds);

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Board -> host: demux SLIP frames from console text. */
        if (FD_ISSET(ser, &rfds)) {
            uint8_t buf[BRIDGE_MTU];
            ssize_t n = read(ser, buf, sizeof(buf));
            if (n <= 0 && !(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                break;  /* serial gone */
            }
            for (ssize_t i = 0; i < n; i++) {
                uint8_t b = buf[i];
                if (b == SLIP_END) {
                    /* END ends a frame and re-arms; a frame starts only at
                     * END + an IPv4 nibble (0x4N) and is validated (version +
                     * total length).  Self-syncing: a dropped byte (J-Link VCOM
                     * under load) costs one frame, not a wedged toggle phase
                     * that drops every frame after it. */
                    if (in_frame && flen > 0) {
                        uint8_t ip[BRIDGE_MTU * 2];
                        size_t iplen = slip_unescape(frame, flen, ip);
                        if (iplen >= 20 && (ip[0] & 0xF0) == 0x40 &&
                            (((size_t)ip[2] << 8) | ip[3]) == iplen) {
                            utun_write(utun, ip, iplen);
                        }
                    }
                    in_frame = 0;
                    flen = 0;
                    slip_armed = 1;
                } else if (slip_armed && (b & 0xF0) == 0x40) {
                    slip_armed = 0;
                    in_frame = 1;
                    flen = 0;
                    frame[flen++] = b;
                } else if (in_frame) {
                    if (flen < sizeof(frame)) {
                        frame[flen++] = b;
                    }
                } else {
                    slip_armed = 0;
                    (void)write(STDOUT_FILENO, &b, 1);  /* console text */
                }
            }
        }

        /* Kernel packet -> board: strip the 4-byte AF header, SLIP-encode. */
        if (FD_ISSET(utun, &rfds)) {
            uint8_t buf[BRIDGE_MTU + 4];
            ssize_t n = read(utun, buf, sizeof(buf));
            if (n > 4) {
                uint8_t out[(BRIDGE_MTU * 2) + 2];
                size_t olen = slip_encode(buf + 4, (size_t)(n - 4), out);
                /* drain fully: the non-blocking fd would otherwise drop the
                 * tail of a burst (e.g. a TLS flight over SLIP), truncating
                 * the frame -- the same bug fixed in gui.c ser_write() */
                size_t off = 0;
                int spins = 0;
                while (off < olen && spins < 3000) {
                    ssize_t w = write(ser, out + off, olen - off);
                    if (w > 0)      { off += (size_t)w; spins = 0; }
                    else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        usleep(1000); spins++;
                    } else break;
                }
            }
        }

        /* Keystrokes -> board shell. */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            uint8_t k[64];
            ssize_t n = read(STDIN_FILENO, k, sizeof(k));
            if (n <= 0) {
                break;
            }
            int quit = 0;
            for (ssize_t i = 0; i < n; i++) {
                if (k[i] == 0x1D) {  /* Ctrl-] */
                    quit = 1;
                }
            }
            (void)write(ser, k, (size_t)n);
            if (quit) {
                break;
            }
        }
    }

    tty_restore();
    bridge_run("ifconfig %s down 2>/dev/null", utun_name);
    return 0;
}
