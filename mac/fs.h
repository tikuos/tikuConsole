/*
 * fs.h - tikuOS /data file access over the shell console (request/response).
 *
 * GUI-independent: every call is a synchronous exchange on an open, raw,
 * O_NONBLOCK serial fd (the one serial_open() returns).  The caller must make
 * sure nothing else is draining the fd for the duration of a call -- in the
 * GTK app that means pausing the console read-watch around each operation.
 *
 * Transfers ride the device's length-prefixed `send`/`recv` commands, so files
 * round-trip byte-exact (embedded \n, \r, NUL) up to one store slot.
 *
 * Authors: WEISER Research Group, National University of Singapore
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKUCONSOLE_FS_H
#define TIKUCONSOLE_FS_H

#include <stddef.h>
#include <stdint.h>

/* One TFS slot on Ambiq.  The device enforces the true per-part cap (512 B on
 * MSP430) and reports it; this is just a host-side sanity bound. */
#define FS_SLOT_MAX 4096

/* Run a text shell command and capture the reply (echoed command line and the
 * trailing prompt stripped) into out[].  Returns reply length. */
int  fs_shell(int fd, const char *line, char *out, size_t outsz);

/* Convenience wrappers over fs_shell(). */
int  fs_ls(int fd, char *out, size_t outsz);              /* "ls /data" */
int  fs_df(int fd, char *out, size_t outsz);              /* "df"       */
int  fs_rm(int fd, const char *name, char *out, size_t outsz);

/* Parse `ls /data` text into filenames (the last token of each non-error
 * line).  Mutates ls_text in place; names[] point into it.  Returns count. */
int  fs_parse_names(char *ls_text, const char *names[], int max);

/* Read /data/<name> into buf[] via the `send` handshake.  Returns the byte
 * count (>= 0), or -1 with err[] filled. */
long fs_get(int fd, const char *name, uint8_t *buf, size_t bufsz,
            char *err, size_t esz);

/* Write len bytes to /data/<name> via the `recv` handshake (0-length falls
 * back to touch).  Returns 0 on success, -1 with err[] filled. */
int  fs_put(int fd, const char *name, const uint8_t *data, size_t len,
            char *err, size_t esz);

#endif /* TIKUCONSOLE_FS_H */
