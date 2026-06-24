#!/usr/bin/env python3
"""
tikufs -- interrogate, read, write and delete files in a tikuOS device's /data
file store over the shell console (serial).

The device exposes the carved-NVM file store via the shell (ls / df / cat / rm /
write), so this host tool just drives those commands over the UART/VCOM and
parses the replies.  No device firmware change is needed for ls / df / get / rm.
Writing larger or multi-line files needs the device-side `recv` command (the
shell `write` is a single line); `put` here handles small single-line content
and tells you when a file needs `recv`.

Usage:
  tikufs --port /dev/cu.usbmodemXXXX ls            # list /data + usage
  tikufs --port ... df                             # store usage
  tikufs --port ... get cfg.json [out.json]        # read a file (-> stdout/host)
  tikufs --port ... put hello.txt note.txt         # write a small text file
  tikufs --port ... rm note.txt                    # delete a file

Authors: WEISER Research Group, National University of Singapore
SPDX-License-Identifier: Apache-2.0
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("tikufs: needs pyserial (pip install pyserial)")

DATA = "/data"
_PROMPT = "tikuOS:/>"      # learned from the live device at startup


def _drain(ser, idle=1.0):
    out = b""
    t0 = time.time()
    while time.time() - t0 < idle:
        d = ser.read(4096)
        if d:
            out += d
            t0 = time.time()
    return out.decode("latin-1", "replace")


def _capture_prompt(ser):
    """Send a bare CR and learn the device's exact prompt string, so we can
    strip it even when file content is glued to it (a `cat` of a file with no
    trailing newline)."""
    global _PROMPT
    ser.reset_input_buffer()
    ser.write(b"\r")
    ser.flush()
    time.sleep(0.5)
    out = _drain(ser, 0.6).replace("\r", "")
    lines = [ln for ln in out.split("\n") if ln.strip()]
    if lines:
        j = lines[-1].rfind(">")
        if j >= 0:
            _PROMPT = lines[-1][:j + 1]


def cmd(ser, line, settle=0.6, idle=1.0):
    """Send a shell command; return the reply body (echoed command + the
    trailing prompt stripped)."""
    ser.reset_input_buffer()
    ser.write((line + "\r").encode())
    ser.flush()
    time.sleep(settle)
    out = _drain(ser, idle).replace("\r", "")
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
    body = cmd(ser, "cat %s/%s" % (DATA, args.name), idle=1.5)
    if body.startswith(("cat:", "read:")) or "cannot" in body.lower():
        sys.stderr.write("tikufs: %s\n" % body)
        return 1
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(body)
        print("tikufs: %d bytes -> %s" % (len(body), args.out))
    else:
        sys.stdout.write(body)
        if body and not body.endswith("\n"):
            sys.stdout.write("\n")
    return 0


def op_rm(ser, args):
    body = cmd(ser, "rm %s/%s" % (DATA, args.name))
    if "cannot" in body.lower():
        sys.stderr.write("tikufs: %s\n" % body)
        return 1
    print("tikufs: removed %s/%s" % (DATA, args.name))
    return 0


def op_put(ser, args):
    with open(args.src, "r") as fh:
        content = fh.read()
    dev = args.name or args.src.rsplit("/", 1)[-1]
    # The shell `write` is a single line: no newlines, bounded length. Larger /
    # multi-line files need the device-side `recv` command (not yet present).
    if "\n" in content.rstrip("\n") or len(content) > 200:
        sys.stderr.write(
            "tikufs: '%s' is multi-line or >200 B; the shell `write` is a single\n"
            "        line. Transfer needs the device `recv` command (planned).\n"
            % args.src)
        return 2
    body = cmd(ser, "write %s/%s %s" % (DATA, dev, content.strip()), idle=1.2)
    if "cannot" in body.lower():
        sys.stderr.write("tikufs: %s\n" % body)
        return 1
    print("tikufs: wrote %d bytes -> %s/%s" % (len(content.strip()), DATA, dev))
    return 0


def main():
    ap = argparse.ArgumentParser(prog="tikufs", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial device (e.g. /dev/cu.usbmodemXXXX)")
    ap.add_argument("--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="op", required=True)
    sub.add_parser("ls", help="list /data")
    sub.add_parser("df", help="store usage")
    g = sub.add_parser("get", help="read a file")
    g.add_argument("name")
    g.add_argument("out", nargs="?", help="host file (default: stdout)")
    p = sub.add_parser("put", help="write a small text file")
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
