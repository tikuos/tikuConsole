"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.nat - NatMixin: board -> internet NAT (needs root).

Installs/removes the iptables MASQUERADE + FORWARD rules (and ip_forward /
rp_filter sysctls) that let the board reach the internet through the host's
WAN interface.  Drives the Internet status light.

SPDX-License-Identifier: Apache-2.0
"""
import socket

from gi.repository import GLib

from tcon import SUBNET, HOST_IP
from tcon.packets import build_ip, build_udp
from slmux import slip_encode

# Host address in 4-byte network order, for the relay's "is this destined for
# the world rather than for us?" check.
_HOST_BYTES = socket.inet_aton(HOST_IP)

# The board's link MTU.  DNS replies larger than this are dropped by the board's
# IP layer, so the gateway trims them to fit (see _dns_fit_packet) -- the board
# only ever keeps the first A record anyway.
_BOARD_MTU = 128


class NatMixin:
    def on_nat(self, _sw, active):
        wan = self._wan_iface()
        op = "-A" if active else "-D"
        rules = [
            ["iptables", "-t", "nat", op, "POSTROUTING", "-s", SUBNET,
             "-o", wan, "-j", "MASQUERADE"],
            ["iptables", op, "FORWARD", "-i", "tun0", "-o", wan, "-j", "ACCEPT"],
            ["iptables", op, "FORWARD", "-i", wan, "-o", "tun0", "-m", "conntrack",
             "--ctstate", "RELATED,ESTABLISHED", "-j", "ACCEPT"],
        ]
        try:
            if active:
                for kv in ("net.ipv4.ip_forward=1", "net.ipv4.conf.all.rp_filter=0",
                           "net.ipv4.conf.tun0.rp_filter=0"):
                    self._run(["sysctl", "-w", kv])
                for cmd in rules:
                    self._run(cmd)
                self.append("[nat] ON via %s  (ip_forward + rp_filter off + "
                            "MASQUERADE + FORWARD)\n" % wan)
                self._set_status("NAT on via %s -- ping 8.8.8.8 from the board"
                                 % wan)
                self.nat_on = True
            else:
                for cmd in rules:
                    try:
                        self._run(cmd)
                    except Exception:
                        pass
                self.append("[nat] OFF\n")
                self._set_status("NAT off")
                self.nat_on = False
        except Exception as e:
            detail = ""
            err = getattr(e, "stderr", None)
            if err:
                detail = err.decode(errors="replace") if isinstance(err, bytes) \
                    else str(err)
            self.append("[nat] ERROR via %s: %s\n" % (wan, (detail or str(e)).strip()))
            self._set_status("NAT error -- see console", err=True)
            self.nat_on = False
        self._update_leds()
        return False

    @staticmethod
    def _wan_iface():
        import subprocess
        try:
            out = subprocess.run(["ip", "route", "get", "8.8.8.8"],
                                 capture_output=True, text=True).stdout
            toks = out.split()
            return toks[toks.index("dev") + 1]
        except Exception:
            return "eth0"

    # ---- rootless UDP relay (no TUN / no root) ----------------------------
    #
    # When the kernel TUN+iptables NAT above is not in use (no root), the board
    # can still reach the internet for request/reply UDP -- DNS, NTP, etc. --
    # via ordinary host sockets: read the board's off-link UDP datagram, send
    # it from a normal socket, and frame the reply back over SLIP.  No
    # privileges required.  Called from ConnectionMixin._on_ip_packet for every
    # board->host packet when there is no TUN; a no-op unless the packet is an
    # off-link IPv4 UDP datagram.

    def _relay_udp(self, pkt):
        """Forward one off-link UDP datagram from the board to the internet.

        Rootless, so UDP only (DNS / NTP / ...).  ICMP (ping) cannot be relayed
        without root; flag it so a silent ping timeout doesn't look like a bug."""
        if len(pkt) < 20 or (pkt[0] >> 4) != 4:
            return                                   # not IPv4
        dst_ip = bytes(pkt[16:20])
        if dst_ip == _HOST_BYTES:
            return                                   # for the host, not the world
        if pkt[9] == 1:                              # ICMP -- needs root (TUN/NAT)
            self._relay_icmp_hint(dst_ip)
            return
        if pkt[9] != 17 or len(pkt) < 28:
            return                                   # only UDP rides this relay
        ihl = (pkt[0] & 0x0f) * 4
        if len(pkt) < ihl + 8:
            return
        src_ip = bytes(pkt[12:16])
        src_port = (pkt[ihl] << 8) | pkt[ihl + 1]
        dst_port = (pkt[ihl + 2] << 8) | pkt[ihl + 3]
        payload = bytes(pkt[ihl + 8:])
        dst_str = socket.inet_ntoa(dst_ip)
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setblocking(False)
            s.connect((dst_str, dst_port))
            s.send(payload)
        except OSError as e:
            self.append("[relay] %s:%d unreachable (%s)\n"
                        % (dst_str, dst_port, e))
            return
        self.append("[relay] board -> %s:%d (%dB)\n"
                    % (dst_str, dst_port, len(payload)))
        watch = GLib.unix_fd_add_full(
            GLib.PRIORITY_DEFAULT, s.fileno(), GLib.IOCondition.IN,
            self._relay_reply, (s, dst_ip, dst_port, src_ip, src_port))
        # Reap the socket if no reply arrives within a few seconds.
        GLib.timeout_add_seconds(6, self._relay_expire, s, watch)

    def _relay_icmp_hint(self, dst_ip):
        """Rate-limited note that ICMP (ping) can't ride the rootless relay."""
        now = GLib.get_monotonic_time()
        if now - getattr(self, "_icmp_hint_t", 0) < 5_000_000:   # ~1 per 5 s
            return
        self._icmp_hint_t = now
        self.append("[relay] ICMP to %s needs root -- ping works only with the "
                    "TUN/NAT bridge; relaunch tikuConsole with sudo (UDP "
                    "services like ntp/dns work rootless).\n"
                    % socket.inet_ntoa(dst_ip))

    def _relay_reply(self, _fd, _cond, data):
        """A reply arrived on a relay socket: frame it back to the board."""
        s, dst_ip, dst_port, src_ip, src_port = data
        try:
            reply = s.recv(2048)
        except OSError:
            return GLib.SOURCE_REMOVE
        udp = build_udp(dst_ip, dst_port, src_ip, src_port, reply)
        pkt = self._dns_fit_packet(build_ip(dst_ip, src_ip, 17, udp))
        try:
            self.ser.write(slip_encode(pkt))
            self.fr_out += 1
            self.by_out += len(pkt)
            self.append("[relay] %s:%d -> board (%dB)\n"
                        % (socket.inet_ntoa(dst_ip), dst_port, len(reply)))
        except Exception:
            pass
        try:
            s.close()
        except OSError:
            pass
        return GLib.SOURCE_REMOVE

    def _relay_expire(self, s, watch):
        """Reap a relay socket whose reply never came (keeps fds bounded)."""
        if s.fileno() != -1:                         # not already closed on reply
            try:
                GLib.source_remove(watch)
            except Exception:
                pass
            try:
                s.close()
            except OSError:
                pass
        return False

    # ---- DNS adaptation for the constrained link -------------------------
    #
    # The board's MTU is 128 bytes, but a popular name (google.com) answers
    # with six A records -- a 152-byte frame the board's IP layer drops, so the
    # query just times out.  Nothing on the UDP path can shrink it (EDNS sizes
    # below 512 are clamped per RFC 6891).  As the gateway onto the constrained
    # link, rewrite an oversize DNS reply to its first A record before framing
    # it -- exactly what a 6LoWPAN/IoT border router does, and lossless for the
    # board, which keeps only one address anyway.

    @staticmethod
    def _dns_skip_name(b, pos):
        """Advance past a DNS name (labels or a compression pointer)."""
        while pos < len(b):
            n = b[pos]
            if (n & 0xc0) == 0xc0:                   # compression pointer (2 B)
                return pos + 2 if pos + 2 <= len(b) else None
            if n == 0:                               # root label ends the name
                return pos + 1
            pos += 1 + n
        return None

    def _dns_trim(self, dns):
        """Rebuild a DNS response as header + question + first A record.

        Returns the smaller message, or None if it is not a clean A-record
        answer we can shrink."""
        if len(dns) < 12:
            return None
        flags = (dns[2] << 8) | dns[3]
        if not (flags & 0x8000) or (flags & 0x000f):  # response, RCODE == 0
            return None
        qd = (dns[4] << 8) | dns[5]
        an = (dns[6] << 8) | dns[7]
        if qd != 1 or an < 1:
            return None
        pos = self._dns_skip_name(dns, 12)
        if pos is None or pos + 4 > len(dns):
            return None
        q_end = pos + 4                              # + QTYPE + QCLASS
        question = bytes(dns[12:q_end])
        pos = q_end
        for _ in range(an):
            npos = self._dns_skip_name(dns, pos)
            if npos is None or npos + 10 > len(dns):
                return None
            rtype = (dns[npos] << 8) | dns[npos + 1]
            rclass = (dns[npos + 2] << 8) | dns[npos + 3]
            ttl = bytes(dns[npos + 4:npos + 8])
            rdlen = (dns[npos + 8] << 8) | dns[npos + 9]
            rdata = npos + 10
            if rtype == 1 and rclass == 1 and rdlen == 4 and rdata + 4 <= len(dns):
                ip = bytes(dns[rdata:rdata + 4])
                hdr = bytes(dns[0:4]) + b"\x00\x01\x00\x01\x00\x00\x00\x00"
                ans = (b"\xc0\x0c\x00\x01\x00\x01" + ttl   # name ptr, A, IN
                       + b"\x00\x04" + ip)                # RDLENGTH 4 + addr
                return hdr + question + ans
            pos = rdata + rdlen
        return None

    def _dns_fit_packet(self, pkt):
        """If @p pkt is a board-bound DNS reply too big for the link MTU,
        return a trimmed copy (first A record only); else return it as-is."""
        if len(pkt) <= _BOARD_MTU or (pkt[0] >> 4) != 4 or pkt[9] != 17:
            return pkt
        ihl = (pkt[0] & 0x0f) * 4
        if len(pkt) < ihl + 8 or ((pkt[ihl] << 8) | pkt[ihl + 1]) != 53:
            return pkt                               # not a UDP/53 (DNS) reply
        trimmed = self._dns_trim(bytes(pkt[ihl + 8:]))
        if trimmed is None:
            return pkt
        dst_port = (pkt[ihl + 2] << 8) | pkt[ihl + 3]
        udp = build_udp(bytes(pkt[12:16]), 53, bytes(pkt[16:20]), dst_port,
                        trimmed)
        new = build_ip(bytes(pkt[12:16]), bytes(pkt[16:20]), 17, udp)
        self.append("[relay] DNS reply %dB > %dB MTU -- trimmed to first A "
                    "record (%dB)\n" % (len(pkt), _BOARD_MTU, len(new)))
        return new
