#!/usr/bin/env python3
"""
TikuConsole v0.01
Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org

Authors: Ambuj Varshney <ambuj@tiku-os.org>

tikuconsole.py - launcher for the GTK4 serial console (a picocom replacement)

A branded desktop terminal for any TikuOS board.  It auto-detects the serial
port, the platform (MSP430 / RP2350 / Apollo / Nordic) and its baud, then gives you a
colour console you can type straight into -- no picocom/minicom needed.

Flip on "Networking" and it additionally brings up SLIP/IP over the very same
wire: a TUN interface (so the Linux kernel's own ping/curl ride it), a ping
panel with a rootless ICMP-over-SLIP board pinger, and board->internet NAT.
That mode is the GUI twin of slmux.py and reuses its SLIP framing.

This file is a thin entry point; the implementation lives in the tcon/ package
(one module per subsystem -- console, connection, ping, nat, leds, splash, ui;
pure helpers in tcon.packets and tcon.ports).

  python3 tikuconsole.py          # plain console -- no root needed
  sudo python3 tikuconsole.py     # only for the host TUN/NAT bridge

Headless smoke test (build the window and quit):
  TIKUCONSOLE_SMOKE_MS=1200 xvfb-run -a python3 tikuconsole.py
List detected ports + platform guesses and exit (no display needed):
  TIKUCONSOLE_SCAN=1 python3 tikuconsole.py

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

SPDX-License-Identifier: Apache-2.0
"""
import os
import sys

sys.dont_write_bytecode = True  # avoid root-owned __pycache__ when run via sudo
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

if os.environ.get("TIKUCONSOLE_SCAN"):          # headless port dump, no GTK/display
    from tcon.ports import scan_and_print
    sys.exit(scan_and_print())

from tcon.app import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
