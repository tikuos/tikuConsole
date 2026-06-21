"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.nat - NatMixin: board -> internet NAT (needs root).

Installs/removes the iptables MASQUERADE + FORWARD rules (and ip_forward /
rp_filter sysctls) that let the board reach the internet through the host's
WAN interface.  Drives the Internet status light.

SPDX-License-Identifier: Apache-2.0
"""
from tcon import SUBNET


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
