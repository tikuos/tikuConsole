"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.packets - ICMP-over-SLIP ping: build/parse packets in userspace.

No TUN, no root, no system 'ping' -- the board's own net stack answers echo
requests.  Pure functions (struct only); no GTK, so they unit-test trivially.

SPDX-License-Identifier: Apache-2.0
"""
import struct


def inet_checksum(data):
    """16-bit one's-complement Internet checksum (RFC 1071)."""
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xffff)
    s += s >> 16
    return (~s) & 0xffff


def build_icmp_echo(ident, seq, payload=b""):
    """ICMP echo request (type 8, code 0) with checksum."""
    head = struct.pack("!BBHHH", 8, 0, 0, ident & 0xffff, seq & 0xffff)
    cks = inet_checksum(head + payload)
    return struct.pack("!BBHHH", 8, 0, cks, ident & 0xffff,
                       seq & 0xffff) + payload


def build_ip(src, dst, proto, payload, ident=0):
    """Minimal 20-byte IPv4 header (no options) + payload; src/dst are 4-byte
    network-order addresses from socket.inet_aton()."""
    fields = (0x45, 0, 20 + len(payload), ident & 0xffff, 0, 64, proto)
    base = struct.pack("!BBHHHBBH4s4s", *fields, 0, src, dst)
    return struct.pack("!BBHHHBBH4s4s", *fields,
                       inet_checksum(base), src, dst) + payload


def parse_icmp_echo_reply(pkt, ident):
    """seq if pkt is an ICMP echo reply (type 0) for our ident, else None.
    IHL-aware; the board already validated the request checksum."""
    if len(pkt) < 28 or (pkt[0] >> 4) != 4 or pkt[9] != 1:
        return None                                # short / not IPv4 / not ICMP
    icmp = pkt[(pkt[0] & 0x0f) * 4:]
    if len(icmp) < 8 or icmp[0] != 0:              # type 0 = echo reply
        return None
    rid, rseq = struct.unpack("!HH", icmp[4:8])
    return rseq if rid == (ident & 0xffff) else None
