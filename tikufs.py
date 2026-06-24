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

try:
    import serial
except ImportError:
    sys.exit("tikufs: needs pyserial (pip install pyserial)")

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


def cmd(ser, line, settle=0.6, idle=1.0):
    """Send a shell command; return the reply body as text (echoed command and
    trailing prompt stripped). For ls / df / rm -- not for binary transfer."""
    ser.reset_input_buffer()
    ser.write((line + "\r").encode())
    ser.flush()
    time.sleep(settle)
    out = _drain(ser, idle).decode("latin-1", "replace").replace("\r", "")
    idx = out.rfind(_PROMPT)                      # cut at the (last) prompt
    if idx >= 0:
        out = out[:idx]
    lines = out.split("\n")
    if lines and lines[0].strip() == line.strip():
        lines = lines[1:]                        # drop the echoed command
    return "\n".join(lines).strip("\n")


# --------------------------------------------------------------------------
# operations
# --------------------------------------------------------------------------

def op_ls(ser, args):
    body = cmd(ser, "ls " + DATA)
    print(body if body else "(empty)")
    return 0


def op_df(ser, args):
    print(cmd(ser, "df"))
    return 0


def op_get(ser, args):
    """Read a file via the device `send` handshake: "send: N\\n" then N raw
    bytes. Binary-safe and exact-length (handles embedded \\n, \\r, NUL)."""
    path = "%s/%s" % (DATA, args.name)
    ser.reset_input_buffer()
    ser.write(("send %s\r" % path).encode())
    ser.flush()
    buf = _read_until(ser, [b"send:"], 3.0)

    # Complete the "send: N\n" header line (or detect an error line).
    m = re.search(rb"send:\s*(\d+)\r?\n", buf)
    t0 = time.time()
    while not m and time.time() - t0 < 2.0:
        buf += ser.read(128) or b""
        m = re.search(rb"send:\s*(\d+)\r?\n", buf)
    if not m:
        em = re.search(rb"send:[^\r\n]*", buf)
        sys.stderr.write("tikufs: %s\n" %
                         (em.group(0).decode("latin-1") if em else "send failed"))
        return 1

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
        sys.stderr.write("tikufs: short read (%d/%d bytes)\n" % (len(data), n))
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
    """Write a file via the device `recv` handshake: "recv <p> N", wait for
    "recv: ready N", stream N raw bytes. Binary-safe up to one store slot."""
    with open(args.src, "rb") as fh:
        data = fh.read()
    dev = args.name or os.path.basename(args.src)
    path = "%s/%s" % (DATA, dev)
    n = len(data)

    # recv requires N >= 1; an empty file is just a create -> use touch.
    if n == 0:
        body = cmd(ser, "touch %s" % path)
        if "cannot" in body.lower():
            sys.stderr.write("tikufs: %s\n" % body)
            return 1
        print("tikufs: wrote 0 bytes -> %s" % path)
        return 0

    ser.reset_input_buffer()
    ser.write(("recv %s %d\r" % (path, n)).encode())
    ser.flush()
    # The device prints "recv: ready N" once it is in the read loop; until then
    # it may reject (length too big, bad usage). Wait for either.
    buf = _read_until(ser, [b"ready", b"must be", b"Usage"], 3.0)
    if b"ready" not in buf:
        em = re.search(rb"recv:[^\r\n]*", buf.replace(b"\r", b""))
        sys.stderr.write("tikufs: %s\n" %
                         (em.group(0).decode("latin-1") if em
                          else "device did not accept recv (need a newer build?)"))
        return 1

    time.sleep(0.15)                              # device is now in its read loop
    ser.write(data)
    ser.flush()
    conf = _read_until(ser, [b"bytes", b"failed", b"timeout"], 5.0)
    cm = re.search(rb"recv:\s*(\d+)\s*bytes", conf)
    if not cm:
        em = re.search(rb"recv:[^\r\n]*", conf.replace(b"\r", b""))
        sys.stderr.write("tikufs: %s\n" %
                         (em.group(0).decode("latin-1") if em else "transfer failed"))
        return 1
    print("tikufs: wrote %d bytes -> %s" % (int(cm.group(1)), path))
    return 0


def op_rm(ser, args):
    body = cmd(ser, "rm %s/%s" % (DATA, args.name))
    if "cannot" in body.lower():
        sys.stderr.write("tikufs: %s\n" % body)
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
