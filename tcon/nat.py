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
        """Forward one off-link UDP datagram from the board to the internet."""
        if len(pkt) < 28 or (pkt[0] >> 4) != 4 or pkt[9] != 17:
            return                                   # not IPv4 UDP
        dst_ip = bytes(pkt[16:20])
        if dst_ip == _HOST_BYTES:
            return                                   # for the host, not the world
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

    def _relay_reply(self, _fd, _cond, data):
        """A reply arrived on a relay socket: frame it back to the board."""
        s, dst_ip, dst_port, src_ip, src_port = data
        try:
            reply = s.recv(2048)
        except OSError:
            return GLib.SOURCE_REMOVE
        udp = build_udp(dst_ip, dst_port, src_ip, src_port, reply)
        pkt = build_ip(dst_ip, src_ip, 17, udp)
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
