/*
 * TikuConsole -- carry a board's BLE link to a desktop on TCP (macOS).
 *
 * The macOS side of TikuBench's ble_peer: this host is the central through
 * CoreBluetooth (ble_mac.m), the desktop is a TCP socket, and the bytes cross
 * as they are, as both links frame their own messages.
 *
 *   ble_relay --name TIKU-DESK --to 127.0.0.1:7749 [--discover-wait 90] [-v]
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "ble_mac.h"

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* All of b[0..n) to fd, which may be non-blocking; 0, or -1 when it fails. */
static int write_all(int fd, const unsigned char *b, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w > 0) {
            b += w;
            n -= (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            usleep(1000);
        } else {
            return -1;
        }
    }
    return 0;
}

/* A TCP connection to host:port, tried until wait_s runs out; -1 if none. */
static int dial(const char *host, const char *port, double wait_s)
{
    double t0 = now_s();
    for (;;) {
        struct addrinfo hints, *ai = NULL, *p;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, port, &hints, &ai) == 0) {
            for (p = ai; p; p = p->ai_next) {
                int s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (s < 0) {
                    continue;
                }
                if (connect(s, p->ai_addr, p->ai_addrlen) == 0) {
                    freeaddrinfo(ai);
                    return s;
                }
                close(s);
            }
            freeaddrinfo(ai);
        }
        if (now_s() - t0 > wait_s) {
            return -1;
        }
        usleep(500 * 1000);
    }
}

int main(int argc, char **argv)
{
    const char *name = "TIKU-DESK";
    char host[256] = "127.0.0.1";
    const char *port = "7749";
    double discover = 90.0;
    int verbose = 0, fd, sock, i;
    long up = 0, down = 0;
    double t0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            name = argv[++i];
        } else if (!strcmp(argv[i], "--to") && i + 1 < argc) {
            char *colon;
            snprintf(host, sizeof host, "%s", argv[++i]);
            colon = strrchr(host, ':');
            if (colon == NULL) {
                fprintf(stderr, "ble_relay: --to wants host:port\n");
                return 2;
            }
            *colon = '\0';
            port = argv[i] + (colon - host) + 1;
        } else if (!strcmp(argv[i], "--discover-wait") && i + 1 < argc) {
            discover = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            verbose = 1;
        } else {
            fprintf(stderr, "usage: ble_relay --name NAME --to HOST:PORT "
                            "[--discover-wait S] [-v]\n");
            return 2;
        }
    }

    fd = ble_mac_open(name);
    if (fd < 0) {
        fprintf(stderr, "ble_relay: no CoreBluetooth transport\n");
        return 2;
    }
    if (verbose) {
        printf("ble_relay: looking for %s\n", name);
        fflush(stdout);
    }
    t0 = now_s();
    while (strcmp(ble_mac_status(), "connected") != 0) {
        const char *st = ble_mac_status();
        if (!strcmp(st, "no radio") || !strcmp(st, "not found") ||
            !strcmp(st, "disconnected") || now_s() - t0 > discover) {
            fprintf(stderr, "ble_relay: %s: %s after %.0f s\n", name, st,
                    now_s() - t0);
            ble_mac_close();
            return 1;
        }
        usleep(50 * 1000);
    }
    if (verbose) {
        printf("ble_relay: %s up in %.1f s\n", name, now_s() - t0);
        fflush(stdout);
    }

    sock = dial(host, port, 30.0);
    if (sock < 0) {
        fprintf(stderr, "ble_relay: nothing listens on %s:%s\n", host, port);
        ble_mac_close();
        return 1;
    }
    for (;;) {
        unsigned char buf[4096];
        fd_set rs;
        struct timeval tv = { 0, 200 * 1000 };
        ssize_t n;
        int r;

        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        FD_SET(sock, &rs);
        r = select((fd > sock ? fd : sock) + 1, &rs, NULL, NULL, &tv);
        if (strcmp(ble_mac_status(), "connected") != 0) {
            break;                              /* the board's link dropped */
        }
        if (r <= 0) {
            continue;
        }
        if (FD_ISSET(sock, &rs)) {              /* desktop -> board */
            n = read(sock, buf, sizeof buf);
            if (n == 0) {
                break;                          /* the desktop hung up */
            }
            if (n > 0) {
                if (write_all(fd, buf, (size_t)n) < 0) {
                    break;
                }
                down++;
            }
        }
        if (FD_ISSET(fd, &rs)) {                /* board -> desktop */
            n = read(fd, buf, sizeof buf);
            if (n == 0) {
                break;
            }
            if (n > 0) {
                if (write_all(sock, buf, (size_t)n) < 0) {
                    break;
                }
                up++;
            }
        }
    }
    close(sock);
    ble_mac_close();
    if (verbose) {
        printf("ble_relay: down; %ld writes to the board, %ld to the desk\n",
               down, up);
    }
    return (up && down) ? 0 : 1;
}
