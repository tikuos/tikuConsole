"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.connection - ConnectionMixin: the serial link and SLIP/IP bridge.

Owns the port picker, open/close of the serial port, the always-on SLIP demux
(console text vs IP packets), the optional kernel TUN bridge (root), and the
live "Networking" apply path shared by the switch and connect.

SPDX-License-Identifier: Apache-2.0
"""
import os
import struct
import fcntl
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib  # noqa: E402

from slmux import slip_encode, slip_unescape, SLIP_END  # noqa: E402
from tcon import BOARD_IP, HOST_IP, TUNSETIFF, IFF_TUN, IFF_NO_PI  # noqa: E402
from tcon.ports import identify_port, scan_ports  # noqa: E402


class ConnectionMixin:
    # ---- port detection ---------------------------------------------------
    def refresh_ports(self, *a):
        self.ports = scan_ports()
        labels = [self._port_label(p) for p in self.ports] or ["(no USB serial ports)"]
        self.port_dd.set_model(Gtk.StringList.new(labels))
        sel = 0
        for i, p in enumerate(self.ports):         # prefer a recognised board
            if identify_port(p)[0] != "unknown":
                sel = i; break
        self.port_dd.set_selected(sel)
        self.on_port_changed()

    @staticmethod
    def _port_label(p):
        plat, _ = identify_port(p)
        return "%s  ·  %s" % (p.device.replace("/dev/", ""), plat)

    def on_port_changed(self, *a):
        i = self.port_dd.get_selected()
        if 0 <= i < len(self.ports):
            p = self.ports[i]
            plat, baud = identify_port(p)
            self.port_path = p.device
            self.platform_lbl.set_text(plat)
            if self.ser is None:                   # don't fight a live session
                self.baud.set_text(str(baud))
        else:
            self.port_path = None
            self.platform_lbl.set_text("--")

    # ---- networking apply (shared by the switch + connect) ----------------
    def on_net_toggle(self, _sw, active):
        # The switch only reflects the user's choice; we never flip it back (a
        # reentrant set_active() from inside this state-set handler leaves GTK's
        # active/state inconsistent and the switch sticks).  The real work lives
        # in _apply_net, shared with on_connect.
        if self.ser is None:                       # not connected: remember choice
            self.netpanel.set_visible(active)
            if active and os.geteuid() != 0:
                self._set_status("Networking mode -- host TUN/NAT needs sudo "
                                 "(SLIP + board ping work without it)")
            return False
        self._apply_net(active)
        return False

    def _apply_net(self, active):
        """Show/hide the networking pane on a live console and bring the host
        TUN up (root) or not.  SLIP and the in-app board ping work over the bare
        serial without root; only the host TUN/NAT bridge needs it."""
        if not active:
            self.netpanel.set_visible(False)
            self.net_hint.set_visible(False)
            self.ping_active = False               # cancel any in-flight ping
            self.send_line("slip off")             # console-only (idempotent)
            self._net_down()
            self.net = False
            self.slip_btn.set_sensitive(False)
            self.nat.set_sensitive(False)
            self.append("[tikuconsole] networking off -- console-only.\n")
            return
        self.netpanel.set_visible(True)            # show the pane
        self.slip_btn.set_sensitive(True)          # SLIP toggle needs no host root
        if os.geteuid() == 0 and self._net_up():
            self.net = True
            self.net_hint.set_visible(False)
            self.nat.set_sensitive(True)
            self.append("[tikuconsole] networking on; enabling SLIP on the "
                        "board...\n")
            GLib.timeout_add(400, self._auto_slip)
            return
        # no host TUN (not root, or setup failed) -- SLIP + ping still work
        self.net = False
        self.nat.set_sensitive(False)
        if os.geteuid() != 0:
            hint = ("⚠ host TUN/NAT bridge needs sudo. SLIP + board ping work "
                    "without it; relaunch with sudo for the full bridge.")
            self._set_status("Networking pane shown -- SLIP + board ping work "
                             "now; host TUN/NAT needs sudo.")
        else:
            hint = "⚠ tun0 setup failed -- see status above"
        self.net_hint.set_markup(
            "<span foreground='#ff6b6b' weight='bold'>%s</span>" % hint)
        self.net_hint.set_visible(True)

    # ---- connect / serial / tun ------------------------------------------
    def on_connect(self, _btn):
        if self.ser is not None:
            self._teardown(); return
        if not self.port_path:
            self._set_status("no serial port -- plug in a board and press ⟳",
                             err=True); return
        try:
            import serial
            self.ser = serial.Serial(self.port_path, int(self.baud.get_text()),
                                     timeout=0)
        except Exception as e:
            self._set_status("error opening %s: %s" % (self.port_path, e),
                             err=True)
            self.ser = None; return
        self.ser_src = GLib.unix_fd_add_full(GLib.PRIORITY_DEFAULT,
                                             self.ser.fileno(),
                                             GLib.IOCondition.IN, self._on_serial)
        self.connect_btn.set_label("Disconnect")
        self.cview.remove_css_class("console-off")  # live: full colour, enterable
        self._update_leds()                        # USB light -> green
        self._set_status("connected to %s @ %s baud" %
                         (self.port_path, self.baud.get_text()))
        GLib.idle_add(self._focus_console)         # grab focus after click settles
        self.append("[tikuconsole] connected (console mode) -- type away.\n")
        if self.net_sw.get_active():               # networking pre-selected
            self._apply_net(True)

    def _net_up(self):
        try:
            self.tun = os.open("/dev/net/tun", os.O_RDWR)
            ifr = struct.pack("16sH", b"tun0", IFF_TUN | IFF_NO_PI)
            fcntl.ioctl(self.tun, TUNSETIFF, ifr)
            self._ip(["addr", "add", HOST_IP, "peer", BOARD_IP, "dev", "tun0"])
            self._ip(["link", "set", "tun0", "up"])
            # Keep the kernel from flooding the tiny board with multicast
            # (SSDP/mDNS) and IPv6 solicitations the instant tun0 appears.
            self._ip(["link", "set", "tun0", "multicast", "off"])
            self._run(["sysctl", "-w", "net.ipv6.conf.tun0.disable_ipv6=1"])
        except Exception as e:
            self._set_status("network setup failed: %s" % e, err=True)
            return False
        self.tun_src = GLib.unix_fd_add_full(GLib.PRIORITY_DEFAULT, self.tun,
                                             GLib.IOCondition.IN, self._on_tun)
        self.tun_lbl.set_text("tun0: up  %s <-> %s" % (HOST_IP, BOARD_IP))
        return True

    def _auto_slip(self):
        self.send_line("slip on")                  # explicit enable (idempotent)
        return GLib.SOURCE_REMOVE

    def _net_down(self):
        """Tear down TUN + NAT only, leaving the serial console running."""
        if self.nat.get_active():
            self.nat.set_active(False)             # fires on_nat -> drops iptables
        if self.tun_src:
            GLib.source_remove(self.tun_src); self.tun_src = 0
        if self.tun >= 0:
            try:
                self._ip(["link", "set", "tun0", "down"])
            except Exception:
                pass
            os.close(self.tun); self.tun = -1
        self.tun_lbl.set_text("tun0: down")

    def _teardown(self):
        if self.ser_src:
            GLib.source_remove(self.ser_src); self.ser_src = 0
        self._net_down()
        self.ping_active = False                    # cancel any in-flight ping
        self.slip_on = False
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.net = False
        self.connect_btn.set_label("Connect")
        self.net_sw.set_sensitive(True)
        self.slip_btn.set_sensitive(False)
        self.nat.set_sensitive(False)
        self._update_leds()                        # USB/SLIP lights -> off
        self._set_status("disconnected")
        self.cview.add_css_class("console-off")     # grey it out: not enterable

    def _on_serial(self, fd, cond, *a):
        try:
            data = self.ser.read(4096)
        except Exception:
            return GLib.SOURCE_REMOVE
        # Always demux SLIP: a frame is delimited by SLIP_END (0xC0), which the
        # board's ASCII console output never contains, so console text falls
        # straight through.  Decoded IP packets go to _on_ip_packet (what lets
        # the rootless board-ping catch replies with no TUN).  Console bytes are
        # batched into runs so append() sees whole strings, not one char at a
        # time -- faster, and lets the SLIP-status detector match.
        text = bytearray()
        for b in data:
            if b == SLIP_END:
                if text:
                    self.append(text.decode("latin-1")); text = bytearray()
                if self.in_frame:
                    if self.frame:
                        self._on_ip_packet(slip_unescape(self.frame))
                    self.frame = bytearray(); self.in_frame = False
                else:
                    self.in_frame = True; self.frame = bytearray()
            elif self.in_frame:
                self.frame.append(b)
            else:
                text.append(b)
        if text:
            self.append(text.decode("latin-1"))
        return GLib.SOURCE_CONTINUE

    def _on_ip_packet(self, pkt):
        """A full SLIP-decoded IP packet from the board."""
        self.fr_in += 1; self.by_in += len(pkt)
        if self.tun >= 0:
            os.write(self.tun, pkt)                 # root path: kernel routes it
        else:
            self._relay_udp(pkt)                    # rootless UDP->internet relay
        if self.ping_active:
            self._ping_rx(pkt)                      # in-app rootless pinger

    def _on_tun(self, fd, cond, *a):
        try:
            pkt = os.read(self.tun, 2048)
        except Exception:
            return GLib.SOURCE_REMOVE
        # Only relay IPv4 unicast addressed to the board; drop the kernel's
        # multicast/broadcast/IPv6 chatter so the tiny board is never flooded.
        if len(pkt) >= 20 and (pkt[0] >> 4) == 4 and \
                pkt[16:20] == bytes((172, 16, 7, 2)):
            pkt = self._dns_fit_packet(pkt)         # shrink oversize DNS replies
            self.ser.write(slip_encode(pkt))
            self.fr_out += 1; self.by_out += len(pkt)
        return GLib.SOURCE_CONTINUE

    # ---- actions / host-system helpers ------------------------------------
    def send_line(self, line):
        if self.ser is not None:
            self.ser.write((line + "\r").encode())

    def _ip(self, args):
        self._run(["ip"] + args)

    @staticmethod
    def _run(args):
        import subprocess
        subprocess.run(args, check=True, capture_output=True)
