"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon - TikuConsole package: shared constants for the GTK4 serial console.

The GUI is assembled in tcon.app from one mixin per subsystem -- console,
connection, ping, nat, leds, splash, ui -- so each lives in its own module
and is easy to read and extend.  Pure, GTK-free helpers live in tcon.packets
(ICMP/IP build+parse) and tcon.ports (serial-port detection).

SPDX-License-Identifier: Apache-2.0
"""
import os as _os

VERSION = "0.01"
GREEN = "#8ae234"

BOARD_IP, HOST_IP, SUBNET = "172.16.7.2", "172.16.7.1", "172.16.7.0/24"

TUNSETIFF = 0x400454CA
IFF_TUN, IFF_NO_PI = 0x0001, 0x1000

# Asset dir (logos / window icon) lives next to the launcher -- the package
# parent -- not inside tcon/.
LOGO_DIR = _os.path.join(
    _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))), "logo")
