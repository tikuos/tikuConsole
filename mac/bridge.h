/*
 * bridge.h - shared TikuOS host-bridge primitives (macOS).
 *
 * The platform-bound core used by both the command-line `slmux` and the GTK
 * `tikuconsole` GUI: SLIP framing, a termios serial port, and a macOS utun
 * network device.  Keeping these in one place means the CLI and the GUI bridge
 * the exact same way.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKUCONSOLE_BRIDGE_H
#define TIKUCONSOLE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

/* --- SLIP framing (RFC 1055 + TikuOS NUL-escape extension) --- */
#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD
#define SLIP_ESC_NUL 0xDE

#define BRIDGE_MTU 2048

/* Encode one IP packet into a SLIP frame (END ... END, escaped).  out must hold
 * up to 2*len + 2 bytes; returns the encoded length. */
size_t slip_encode(const uint8_t *pkt, size_t len, uint8_t *out);

/* Un-escape an accumulated SLIP frame body (no leading/trailing END) into out
 * (which must hold at least len bytes); returns the decoded length. */
size_t slip_unescape(const uint8_t *data, size_t len, uint8_t *out);

/* Open a serial port in raw mode at the given baud.  Returns a non-blocking fd,
 * or -1 with errno set. */
int serial_open(const char *path, unsigned baud);

/* Create a macOS utun device.  Writes the assigned name ("utunN") into
 * name/name_len.  Returns the fd, or -1 with errno set.  Needs root. */
int utun_open(char *name, size_t name_len);

/* Write one IP packet to utun (prepends the required 4-byte AF_INET header). */
void utun_write(int fd, const uint8_t *pkt, size_t len);

/* Run a host configuration command (ifconfig/route), echoing it to stderr.
 * Values come from controlled config, so a plain system() is acceptable. */
int bridge_run(const char *fmt, ...);

/* Best-effort: copy the first /dev/cu.usbmodem* path into buf.  Returns 1 if
 * one was found, 0 otherwise. */
int bridge_default_port(char *buf, size_t len);

#endif /* TIKUCONSOLE_BRIDGE_H */
