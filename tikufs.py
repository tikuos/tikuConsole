#!/usr/bin/env python3
"""
tikufs -- interrogate, read, write and delete files in a tikuOS device's /data
file store over the shell console (serial).

The device exposes the carved-NVM file store through the shell:

  * ls / df / rm  -- text commands, parsed from their replies.
  * send <path>   -- prints "send: N" then streams N raw bytes  (binary-safe).
  * recv <path> N -- prints "recv: ready N" then reads N raw bytes (binary-safe).

`get`/`put` ride the length-prefixed send/recv handshake, so multi-line and
arbitrary binary files round-trip byte-exact up to one store slot
(TIKU_TFS_SLOT_DATA -- 4096 B on Ambiq, 512 B on MSP430).  No escaping, no
single-line `write` limit.

Usage:
  tikufs --port /dev/cu.usbmodemXXXX ls            # list /data + usage
  tikufs --port ... df                             # store usage
  tikufs --port ... get cfg.json [out.json]        # read a file (-> host/stdout)
  tikufs --port ... put blink.bas [name]           # write any file (<= 1 slot)
  tikufs --port ... rm note.txt                    # delete a file

Authors: WEISER Research Group, National University of Singapore
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import os
import re
import sys
import time

# NB: pyserial is imported inside main() (not at module load) so this file can
# be imported purely for its protocol helpers -- e.g. the GUI file browser
# tcon/files.py -- without pulling in serial or exiting when it is absent.

DATA = "/data"
_PROMPT = "tikuOS:/>"      # learned from the live device at startup


# --------------------------------------------------------------------------
# low-level serial helpers
# --------------------------------------------------------------------------

def _drain(ser, idle=1.0):
    """Read until the line goes quiet for `idle` seconds; return raw bytes."""
    out = b""
    t0 = time.time()
    while time.time() - t0 < idle:
        d = ser.read(4096)
        if d:
            out += d
            t0 = time.time()
    return out


def _read_until(ser, markers, timeout=4.0):
    """Accumulate bytes until any marker (bytes) appears, or timeout. Returns
    the buffer collected so far either way."""
    if isinstance(markers, (bytes, bytearray)):
        markers = [markers]
    out = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = ser.read(256)
        if d:
            out += d
            t0 = time.time()
            if any(m in out for m in markers):
                break
    return out


def _capture_prompt(ser):
    """Send a bare CR and learn the device's exact prompt string, so we can
    strip it even when text is glued to it."""
    global _PROMPT
    ser.reset_input_buffer()
    ser.write(b"\r")
    ser.flush()
    time.sleep(0.5)
    out = _drain(ser, 0.6).decode("latin-1", "replace").replace("\r", "")
    lines = [ln for ln in out.split("\n") if ln.strip()]
    if lines:
        j = lines[-1].rfind(">")
        if j >= 0:
            _PROMPT = lines[-1][:j + 1]


_ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")   # CSI escape sequences


def cmd(ser, line, settle=0.6, idle=1.0):
    """Send a shell command; return the reply body as text with the echoed
    command, the shell prompt and ANSI colour stripped.  For ls / df / rm --
    not for binary transfer.

    The prompt is removed structurally (the trailing line that ends in '>')
    rather than by matching a captured string, so this is robust to the colour
    codes the shell wraps its prompt in and to the prompt changing with the
    device's cwd (tikuOS:/> vs tikuOS:/data>)."""
    ser.reset_input_buffer()
    ser.write((line + "\r").encode())
    ser.flush()
    time.sleep(settle)
    raw = _ANSI_RE.sub(b"", _drain(ser, idle))
    out = raw.decode("latin-1", "replace").replace("\r", "")
    lines = out.split("\n")
    if lines and lines[0].strip() == line.strip():
        lines = lines[1:]                        # drop the echoed command
    while lines and not lines[-1].strip():       # trailing blank lines
        lines.pop()
    if lines and lines[-1].strip().endswith(">"):  # the shell prompt line
        lines.pop()
    return "\n".join(lines).strip("\n")


# --------------------------------------------------------------------------
# protocol (pure: operate on a serial.Serial, return data / raise FsError).
# Shared by the CLI ops below and the GUI file browser (tcon/files.py).
# --------------------------------------------------------------------------

SLOT_MAX = 4096          # one TFS slot on Ambiq; the device enforces the real cap


class FsError(Exception):
    """A device-side file operation failed; str(e) is user-facing."""


def parse_names(ls_text):
    """Filenames from `ls /data` text: the last token of each non-error line."""
    names = []
    for ln in ls_text.split("\n"):
        s = ln.strip()
        if not s or "cannot" in ln or ln.startswith("ls:"):
            continue
        if s.endswith(">"):                      # a stray shell prompt line
            continue
        names.append(s.split()[-1])
    return names


def ls(ser):
    """Raw `ls /data` reply text."""
    return cmd(ser, "ls " + DATA)


def df(ser):
    """Raw `df` reply text."""
    return cmd(ser, "df")


def rm(ser, name):
    """Delete /data/<name>. Raises FsError on failure."""
    body = cmd(ser, "rm %s/%s" % (DATA, name))
    if "cannot" in body.lower():
        raise FsError(body or ("cannot remove %s" % name))


def get(ser, name):
    """Read /data/<name> via the `send` handshake. Returns bytes (binary-safe,
    exact-length: embedded \\n, \\r, NUL all survive). Raises FsError."""
    ser.reset_input_buffer()
    ser.write(("send %s/%s\r" % (DATA, name)).encode())
    ser.flush()
    buf = _read_until(ser, [b"send:"], 3.0)
    m = re.search(rb"send:\s*(\d+)\r?\n", buf)
    t0 = time.time()
    while not m and time.time() - t0 < 2.0:
        buf += ser.read(128) or b""
        m = re.search(rb"send:\s*(\d+)\r?\n", buf)
    if not m:
        em = re.search(rb"send:[^\r\n]*", buf)
        raise FsError(em.group(0).decode("latin-1") if em else "send failed")
    n = int(m.group(1))
    data = buf[m.end():]
    t0 = time.time()
    while len(data) < n and time.time() - t0 < 4.0:
        d = ser.read(n - len(data))
        if d:
            data += d
            t0 = time.time()
    data = data[:n]
    if len(data) < n:
        raise FsError("short read (%d/%d bytes)" % (len(data), n))
    return data


def put(ser, name, data):
    """Write bytes to /data/<name> via the `recv` handshake (0-length -> touch).
    Raises FsError on failure."""
    path = "%s/%s" % (DATA, name)
    n = len(data)
    if n == 0:                                    # recv needs N>=1: just create
        body = cmd(ser, "touch %s" % path)
        if "cannot" in body.lower():
            raise FsError(body)
        return
    ser.reset_input_buffer()
    ser.write(("recv %s %d\r" % (path, n)).encode())
    ser.flush()
    # "recv: ready N" once the device is in its read loop; else a rejection.
    buf = _read_until(ser, [b"ready", b"must be", b"Usage"], 3.0)
    if b"ready" not in buf:
        em = re.search(rb"recv:[^\r\n]*", buf.replace(b"\r", b""))
        raise FsError(em.group(0).decode("latin-1") if em
                      else "device did not accept recv (need a newer build?)")
    time.sleep(0.15)                              # device is now in its read loop
    ser.write(data)
    ser.flush()
    conf = _read_until(ser, [b"bytes", b"failed", b"timeout"], 5.0)
    cm = re.search(rb"recv:\s*(\d+)\s*bytes", conf)
    if not cm:
        em = re.search(rb"recv:[^\r\n]*", conf.replace(b"\r", b""))
        raise FsError(em.group(0).decode("latin-1") if em else "transfer failed")


# --------------------------------------------------------------------------
# CLI operations (thin wrappers over the protocol above)
# --------------------------------------------------------------------------

def op_ls(ser, args):
    body = ls(ser)
    print(body if body else "(empty)")
    return 0


def op_df(ser, args):
    print(df(ser))
    return 0


def op_get(ser, args):
    try:
        data = get(ser, args.name)
    except FsError as e:
        sys.stderr.write("tikufs: %s\n" % e)
        return 1
    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(data)
        print("tikufs: %d bytes -> %s" % (len(data), args.out))
    else:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
    return 0


def op_put(ser, args):
    with open(args.src, "rb") as fh:
        data = fh.read()
    dev = args.name or os.path.basename(args.src)
    try:
        put(ser, dev, data)
    except FsError as e:
        sys.stderr.write("tikufs: %s\n" % e)
        return 1
    print("tikufs: wrote %d bytes -> %s/%s" % (len(data), DATA, dev))
    return 0


def op_rm(ser, args):
    try:
        rm(ser, args.name)
    except FsError as e:
        sys.stderr.write("tikufs: %s\n" % e)
        return 1
    print("tikufs: removed %s/%s" % (DATA, args.name))
    return 0


def main():
    ap = argparse.ArgumentParser(prog="tikufs", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial device (e.g. /dev/cu.usbmodemXXXX)")
    ap.add_argument("--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="op", required=True)
    sub.add_parser("ls", help="list /data")
    sub.add_parser("df", help="store usage")
    g = sub.add_parser("get", help="read a file (binary-safe)")
    g.add_argument("name")
    g.add_argument("out", nargs="?", help="host file (default: stdout)")
    p = sub.add_parser("put", help="write a file (binary-safe, <= 1 slot)")
    p.add_argument("src", help="host file")
    p.add_argument("name", nargs="?", help="device name (default: basename)")
    r = sub.add_parser("rm", help="delete a file")
    r.add_argument("name")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        sys.exit("tikufs: needs pyserial (pip install pyserial)")

    ops = {"ls": op_ls, "df": op_df, "get": op_get, "put": op_put, "rm": op_rm}
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.4)
    except serial.SerialException as e:
        sys.exit("tikufs: cannot open %s: %s" % (args.port, e))
    try:
        time.sleep(0.3)
        _capture_prompt(ser)
        return ops[args.op](ser, args)
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
