#!/usr/bin/env python3
"""slmux-gui - GTK4 desktop front-end for the TikuOS one-wire SLIP console.

The GUI twin of tools/slmux.py: it opens the serial port, creates a TUN
interface (so the Linux kernel's networking rides it) and demultiplexes the
single wire into a board console (text) and IP frames (-> tun0), all off the
GLib main loop.  It reuses slmux.py's SLIP framing so the two stay in sync.

  sudo python3 tools/slmux_gui.py            # needs root (creates the TUN)

Headless smoke test (no human, no live port -- builds the window and quits):
  SLMUX_GUI_SMOKE_MS=1200 xvfb-run -a python3 tools/slmux_gui.py

Authors: Ambuj Varshney <ambuj@tiku-os.org>
SPDX-License-Identifier: Apache-2.0
"""
import os
import re
import sys
import struct
import fcntl

sys.dont_write_bytecode = True  # avoid root-owned __pycache__ when run via sudo

import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gdk, GLib, Gio, Pango  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from slmux import slip_encode, slip_unescape, SLIP_END, SLIP_ESC  # noqa: E402

TUNSETIFF = 0x400454CA
IFF_TUN, IFF_NO_PI = 0x0001, 0x1000


class BaudPicker(Gtk.DropDown):
    """Dropdown of common baud rates with a Gtk.Entry-style get_text/set_text."""
    RATES = ["9600", "19200", "38400", "57600",
             "115200", "230400", "460800", "921600"]

    def __init__(self, default="115200"):
        super().__init__(model=Gtk.StringList.new(self.RATES))
        self.set_text(str(default))

    def get_text(self):
        i = self.get_selected()
        return self.RATES[i] if 0 <= i < len(self.RATES) else "115200"

    def set_text(self, s):
        s = str(s).strip()
        self.set_selected(self.RATES.index(s) if s in self.RATES
                          else self.RATES.index("115200"))


class SlmuxGui(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="org.tikuos.slmux")
        self.ser = None
        self.tun = -1
        self.ser_src = 0
        self.tun_src = 0
        # demux state
        self.in_frame = False
        self.frame = bytearray()
        # counters
        self.fr_in = self.fr_out = self.by_in = self.by_out = 0
        # ping
        self.ping_proc = None
        self.ping_rtts = []
        self.ping_sent = self.ping_recv = 0

    # ---- UI ---------------------------------------------------------------
    def do_activate(self):
        win = Gtk.ApplicationWindow(application=self, title="slmux - TikuOS SLIP console")
        win.set_default_size(900, 560)
        win.set_titlebar(Gtk.HeaderBar())   # window controls incl. maximize
        self.win = win

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        root.set_margin_top(8); root.set_margin_bottom(8)
        root.set_margin_start(8); root.set_margin_end(8)
        win.set_child(root)

        # --- top bar: port / baud / connect / status / slip ---
        bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        bar.append(Gtk.Label(label="Port"))
        self.port = Gtk.Entry(text="/dev/ttyACM0"); bar.append(self.port)
        bar.append(Gtk.Label(label="Baud"))
        self.baud = BaudPicker()
        bar.append(self.baud)
        self.connect_btn = Gtk.Button(label="Connect")
        self.connect_btn.connect("clicked", self.on_connect); bar.append(self.connect_btn)
        self.status = Gtk.Label(label="disconnected"); self.status.set_xalign(0)
        self.status.set_selectable(True)
        self.status.set_hexpand(True); bar.append(self.status)
        self.slip_btn = Gtk.Button(label="Toggle SLIP"); self.slip_btn.set_sensitive(False)
        self.slip_btn.connect("clicked", lambda b: self.send_line("slip"))
        bar.append(self.slip_btn)
        root.append(bar)

        # --- split: console | network ---
        paned = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        paned.set_vexpand(True); paned.set_position(600); root.append(paned)

        # console
        cbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        sw = Gtk.ScrolledWindow(); sw.set_vexpand(True)
        self.cview = Gtk.TextView(); self.cview.set_editable(False)
        self.cview.set_monospace(True); self.cview.set_wrap_mode(Gtk.WrapMode.CHAR)
        self.cbuf = self.cview.get_buffer(); sw.set_child(self.cview)
        self.cview.add_css_class("console")
        self._init_console_style()
        kc = Gtk.EventControllerKey()          # type straight into the console
        kc.connect("key-pressed", self._on_console_key)
        self.cview.add_controller(kc)
        self.cview.set_can_focus(True)
        cbox.append(sw)
        self.cin = Gtk.Entry(placeholder_text="optional -- or just click the console above and type")
        self.cin.set_sensitive(False); self.cin.connect("activate", self.on_send)
        cbox.append(self.cin)
        paned.set_start_child(cbox)

        # network panel
        nbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        nbox.set_margin_start(6)
        nbox.append(self._h("Network"))
        self.tun_lbl = Gtk.Label(label="tun0: down"); self.tun_lbl.set_xalign(0)
        nbox.append(self.tun_lbl)
        self.cnt_lbl = Gtk.Label(label="frames in/out: 0/0   bytes: 0/0")
        self.cnt_lbl.set_xalign(0); nbox.append(self.cnt_lbl)

        nbox.append(self._h("Ping (host kernel -> via tun0)"))
        pb = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        self.ping_t = Gtk.Entry(text="172.16.7.2"); self.ping_t.set_hexpand(True)
        self.ping_t.connect("activate", self.on_ping); pb.append(self.ping_t)
        self.ping_btn = Gtk.Button(label="Ping")
        self.ping_btn.connect("clicked", self.on_ping); pb.append(self.ping_btn)
        nbox.append(pb)
        self.ping_stats = Gtk.Label(label="idle -- enter an address and click Ping")
        self.ping_stats.set_xalign(0); self.ping_stats.set_selectable(True)
        nbox.append(self.ping_stats)
        cap = Gtk.Label(label="round-trip time per packet (taller = slower):")
        cap.set_xalign(0); cap.add_css_class("dim-label"); nbox.append(cap)
        self.spark = Gtk.DrawingArea(); self.spark.set_content_height(74)
        self.spark.set_draw_func(self._draw_spark); nbox.append(self.spark)
        psw = Gtk.ScrolledWindow(); psw.set_min_content_height(120); psw.set_vexpand(True)
        self.ping_view = Gtk.TextView(); self.ping_view.set_editable(False)
        self.ping_view.set_monospace(True); self.ping_view.add_css_class("console")
        self.ping_buf = self.ping_view.get_buffer()
        self.ping_ok = self.ping_buf.create_tag("ok", foreground="#8ae234")
        self.ping_bad = self.ping_buf.create_tag("bad", foreground="#ff6b6b")
        psw.set_child(self.ping_view); nbox.append(psw)

        nbox.append(self._h("Internet (NAT)"))
        natb = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        natb.append(Gtk.Label(label="board -> internet"))
        self.nat = Gtk.Switch(); self.nat.connect("state-set", self.on_nat); natb.append(self.nat)
        nbox.append(natb)
        paned.set_end_child(nbox)

        GLib.timeout_add(500, self._refresh_counters)
        win.present()

        smoke = os.environ.get("SLMUX_GUI_SMOKE_MS")
        if smoke:
            GLib.timeout_add(int(smoke), lambda: (self.quit(), False)[1])

    def _h(self, text):
        lbl = Gtk.Label(label=text); lbl.set_xalign(0)
        lbl.set_attributes(self._bold()); lbl.set_margin_top(6)
        return lbl

    @staticmethod
    def _bold():
        a = Pango.AttrList(); a.insert(Pango.attr_weight_new(Pango.Weight.BOLD)); return a

    # ---- connect / serial / tun ------------------------------------------
    def on_connect(self, _btn):
        if self.ser is not None:
            self._teardown(); return
        if os.geteuid() != 0:
            self._set_status("must run as root (TUN) -- use sudo", err=True); return
        try:
            import serial
            self.ser = serial.Serial(self.port.get_text(), int(self.baud.get_text()), timeout=0)
            self.tun = os.open("/dev/net/tun", os.O_RDWR)
            ifr = struct.pack("16sH", b"tun0", IFF_TUN | IFF_NO_PI)
            fcntl.ioctl(self.tun, TUNSETIFF, ifr)
            self._ip(["addr", "add", "172.16.7.1", "peer", "172.16.7.2", "dev", "tun0"])
            self._ip(["link", "set", "tun0", "up"])
            # Keep the kernel from flooding the tiny board with multicast
            # (SSDP/mDNS) and IPv6 solicitations the instant tun0 appears.
            self._ip(["link", "set", "tun0", "multicast", "off"])
            self._run(["sysctl", "-w", "net.ipv6.conf.tun0.disable_ipv6=1"])
        except Exception as e:
            self._set_status("error: %s" % e, err=True); self._teardown(); return
        self.ser_src = GLib.unix_fd_add_full(GLib.PRIORITY_DEFAULT, self.ser.fileno(),
                                             GLib.IOCondition.IN, self._on_serial)
        self.tun_src = GLib.unix_fd_add_full(GLib.PRIORITY_DEFAULT, self.tun,
                                             GLib.IOCondition.IN, self._on_tun)
        self.connect_btn.set_label("Disconnect")
        self.slip_btn.set_sensitive(True); self.cin.set_sensitive(True)
        self.tun_lbl.set_text("tun0: up  172.16.7.1 <-> 172.16.7.2")
        self._set_status("connected to " + self.port.get_text())
        self.append("[slmux] connected; enabling SLIP mode...\n")
        self.cview.grab_focus()                # ready to type straight away
        GLib.timeout_add(400, self._auto_slip)

    def _auto_slip(self):
        self.send_line("slip on")          # explicit enable (idempotent)
        return GLib.SOURCE_REMOVE

    def _teardown(self):
        for s in (self.ser_src, self.tun_src):
            if s:
                GLib.source_remove(s)
        self.ser_src = self.tun_src = 0
        if self.tun >= 0:
            try:
                self._ip(["link", "set", "tun0", "down"])
            except Exception:
                pass
            os.close(self.tun); self.tun = -1
        if self.ser is not None:
            self.ser.close(); self.ser = None
        self.connect_btn.set_label("Connect")
        self.slip_btn.set_sensitive(False); self.cin.set_sensitive(False)
        self.tun_lbl.set_text("tun0: down"); self._set_status("disconnected")

    def _on_serial(self, fd, cond, *a):
        try:
            data = self.ser.read(4096)
        except Exception:
            return GLib.SOURCE_REMOVE
        for b in data:
            if b == SLIP_END:
                if self.in_frame:
                    if self.frame:
                        pkt = slip_unescape(self.frame)
                        os.write(self.tun, pkt)
                        self.fr_in += 1; self.by_in += len(pkt)
                    self.frame = bytearray(); self.in_frame = False
                else:
                    self.in_frame = True; self.frame = bytearray()
            elif self.in_frame:
                self.frame.append(b)
            else:
                self.append(bytes([b]).decode("latin-1"))
        return GLib.SOURCE_CONTINUE

    def _on_tun(self, fd, cond, *a):
        try:
            pkt = os.read(self.tun, 2048)
        except Exception:
            return GLib.SOURCE_REMOVE
        # Only relay IPv4 unicast addressed to the board; drop the kernel's
        # multicast/broadcast/IPv6 chatter (SSDP, mDNS, ...) so the tiny board
        # is never flooded and its console/SLIP stream stays clean.
        if len(pkt) >= 20 and (pkt[0] >> 4) == 4 and pkt[16:20] == bytes((172, 16, 7, 2)):
            self.ser.write(slip_encode(pkt))
            self.fr_out += 1; self.by_out += len(pkt)
        return GLib.SOURCE_CONTINUE

    # ---- console / actions -----------------------------------------------
    def on_send(self, entry):
        self.send_line(entry.get_text()); entry.set_text("")

    def send_line(self, line):
        if self.ser is not None:
            self.ser.write((line + "\r").encode())

    def _on_console_key(self, _ctrl, keyval, _code, state):
        """Type straight into the console -- forward each keystroke to the board,
        which echoes it back, so the console behaves like a real terminal."""
        if self.ser is None:
            return False
        ctrl = bool(state & Gdk.ModifierType.CONTROL_MASK)
        if ctrl and keyval in (Gdk.KEY_c, Gdk.KEY_C):   # copy if selecting, else ^C
            if self.cbuf.get_has_selection():
                return False
            self.ser.write(b"\x03"); return True
        if ctrl and keyval in (Gdk.KEY_v, Gdk.KEY_V):   # paste clipboard to board
            self.cview.get_clipboard().read_text_async(None, self._paste_done)
            return True
        if keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter):
            self.ser.write(b"\r"); return True
        if keyval in (Gdk.KEY_BackSpace, Gdk.KEY_Delete):
            self.ser.write(b"\x08"); return True
        if keyval == Gdk.KEY_Tab:
            self.ser.write(b"\t"); return True
        if keyval == Gdk.KEY_Escape:
            self.ser.write(b"\x1b"); return True
        if ctrl:                                        # other Ctrl-<letter>
            lk = Gdk.keyval_to_lower(keyval)
            if Gdk.KEY_a <= lk <= Gdk.KEY_z:
                self.ser.write(bytes([lk - Gdk.KEY_a + 1])); return True
        uch = Gdk.keyval_to_unicode(keyval)             # printable character
        if uch >= 0x20 and uch != 0x7f:
            self.ser.write(chr(uch).encode("utf-8")); return True
        return False

    def _paste_done(self, clipboard, res):
        try:
            text = clipboard.read_text_finish(res)
        except Exception:
            return
        if text and self.ser is not None:
            self.ser.write(text.replace("\n", "\r").encode("utf-8", "replace"))

    def _init_console_style(self):
        css = Gtk.CssProvider()
        data = ("textview.console, textview.console text {"
                " background-color:#0b0b0b; color:#cccccc; }"
                "textview.console { padding:4px; }")
        try:
            css.load_from_string(data)
        except Exception:
            css.load_from_data(data.encode())
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        palette = {30: "#666666", 31: "#ff6b6b", 32: "#8ae234", 33: "#fce94f",
                   34: "#729fcf", 35: "#ad7fa8", 36: "#34e2e2", 37: "#d3d7cf"}
        self.tags = {}
        for code, color in palette.items():
            self.tags["fg%d" % code] = self.cbuf.create_tag("fg%d" % code,
                                                            foreground=color)
        self.tags["bold"] = self.cbuf.create_tag("bold", weight=Pango.Weight.BOLD)
        self.tags["dim"] = self.cbuf.create_tag("dim", foreground="#7f7f7f")
        self.ansi_pending = ""
        self.cur = []                       # active ANSI tag names

    _CSI = re.compile(r"\x1b\[([0-9;]*)([A-Za-z])")

    def append(self, text):
        s = self.ansi_pending + text
        self.ansi_pending = ""
        # Stash a trailing, incomplete escape for the next chunk.
        i = s.rfind("\x1b")
        if i != -1 and self._CSI.search(s, i) is None and len(s) - i < 24:
            self.ansi_pending = s[i:]; s = s[:i]
        pos = 0
        for m in self._CSI.finditer(s):
            if m.start() > pos:
                self._insert(s[pos:m.start()])
            if m.group(2) == "m":           # SGR (colour); drop other CSI codes
                self._sgr(m.group(1))
            pos = m.end()
        if pos < len(s):
            self._insert(s[pos:])

    def _insert(self, t):
        if not t:
            return
        if "\b" in t or "\r" in t:      # GtkTextView is not a terminal
            self._insert_ctl(t)
        else:
            self._raw_insert(t)

    def _raw_insert(self, t):
        if not t:
            return
        it = self.cbuf.get_end_iter()
        tags = [self.tags[n] for n in self.cur if n in self.tags]
        if tags:
            self.cbuf.insert_with_tags(it, t, *tags)
        else:
            self.cbuf.insert(it, t)
        self.cview.scroll_to_mark(self.cbuf.get_insert(), 0.0, False, 0, 0)

    def _insert_ctl(self, t):
        """Interpret the control bytes a terminal would: the board echoes
        "\\b \\b" to erase, so treat BS (0x08) as delete-previous-char, and
        drop lone CR (the following LF makes the line break)."""
        run = ""
        for ch in t:
            if ch == "\b":
                if run:
                    self._raw_insert(run); run = ""
                end = self.cbuf.get_end_iter()
                start = end.copy()
                if start.backward_char():
                    self.cbuf.delete(start, end)
            elif ch == "\r":
                continue
            else:
                run += ch
        if run:
            self._raw_insert(run)
        self.cview.scroll_to_mark(self.cbuf.get_insert(), 0.0, False, 0, 0)

    def _sgr(self, params):
        for c in ([int(x) for x in params.split(";") if x] or [0]):
            if c == 0:
                self.cur = []
            elif c == 1 and "bold" not in self.cur:
                self.cur.append("bold")
            elif c == 2:
                self.cur = [t for t in self.cur if not t.startswith("fg")] + ["dim"]
            elif 30 <= c <= 37:
                self.cur = [t for t in self.cur if not t.startswith("fg")] + ["fg%d" % c]
            elif 90 <= c <= 97:
                self.cur = [t for t in self.cur if not t.startswith("fg")] + ["fg%d" % (c - 60)]

    def on_ping(self, _w):
        if self.ping_proc is not None:
            return
        target = self.ping_t.get_text().strip()
        if not target:
            return
        self.ping_rtts = []; self.ping_sent = 0; self.ping_recv = 0
        self.ping_buf.set_text(""); self.spark.queue_draw()
        self.ping_stats.set_text("pinging %s ..." % target)
        self.ping_btn.set_sensitive(False)
        try:
            self.ping_proc = Gio.Subprocess.new(
                ["ping", "-O", "-c", "5", "-i", "0.3", "-W", "1", target],
                Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        except Exception as e:
            self.ping_stats.set_text("ping error: %s" % e)
            self.ping_btn.set_sensitive(True); self.ping_proc = None; return
        self.ping_proc.communicate_utf8_async(None, None, self._ping_done)

    def _ping_done(self, proc, res):
        self.ping_proc = None; self.ping_btn.set_sensitive(True)
        try:
            _, out, _ = proc.communicate_utf8_finish(res)
        except Exception as e:
            self.ping_stats.set_text("ping error: %s" % e); return
        out = out or ""
        rows = []                                  # (seq, rtt|None)
        for line in out.splitlines():
            m = re.search(r"icmp_seq=(\d+).*time=([\d.]+)", line)
            if m:
                rows.append((int(m.group(1)), float(m.group(2))))
            elif "no answer yet" in line:
                m2 = re.search(r"icmp_seq=(\d+)", line)
                rows.append((int(m2.group(1)) if m2 else len(rows) + 1, None))
            else:
                mt = re.search(r"(\d+) packets transmitted", line)
                if mt:
                    self.ping_sent = max(self.ping_sent, int(mt.group(1)))
        self.ping_rtts = [rt for _, rt in rows if rt is not None]
        self.ping_sent = max(self.ping_sent, len(rows))
        self.ping_recv = len(self.ping_rtts)
        if not rows:                               # ping errored -> show why
            self._ping_row(out.strip()[-300:] or "(no output)", self.ping_bad)
        mx = max(self.ping_rtts) if self.ping_rtts else 1.0
        for seq, rt in rows:                       # one row per packet, with a bar
            if rt is None:
                self._ping_row("packet %-3d  no reply (timed out)" % seq, self.ping_bad)
            else:
                bar = "█" * max(1, int(round(20 * rt / mx)))
                self._ping_row("packet %-3d %7.1f ms  %s" % (seq, rt, bar),
                               self.ping_ok)
        self._ping_stats(); self.spark.queue_draw()

    def _ping_row(self, text, tag):
        self.ping_buf.insert_with_tags(self.ping_buf.get_end_iter(), text + "\n", tag)
        self.ping_view.scroll_to_mark(self.ping_buf.get_insert(), 0, False, 0, 0)

    def _ping_stats(self):
        r = self.ping_rtts
        sent = self.ping_sent
        loss = int(round(100 * (sent - len(r)) / sent)) if sent else 0
        if r:
            self.ping_stats.set_text(
                "%d sent · %d received · %d%% lost     "
                "round-trip min %.1f / avg %.1f / max %.1f ms"
                % (sent, len(r), loss, min(r), sum(r) / len(r), max(r)))
        elif sent:
            self.ping_stats.set_text(
                "%d sent · 0 received · 100%% lost  -- no replies "
                "(is SLIP on / NAT needed?)" % sent)
        else:
            self.ping_stats.set_text("idle -- enter an address and click Ping")

    def _draw_spark(self, area, cr, w, h, *a):
        cr.set_source_rgb(0.04, 0.04, 0.04); cr.paint()
        cr.select_font_face("monospace")
        cr.set_font_size(10)
        r = self.ping_rtts
        if not r:
            cr.set_source_rgb(0.45, 0.45, 0.45)
            msg = "no data yet -- click Ping to chart round-trip time"
            ext = cr.text_extents(msg)
            cr.move_to((w - ext.width) / 2.0, h / 2.0 + 4); cr.show_text(msg)
            return
        pad_l, pad_t, pad_b = 38.0, 4.0, 13.0      # axis margins
        pw, ph = max(1.0, w - pad_l - 6), max(1.0, h - pad_t - pad_b)
        hi = max(r); n = len(r)
        scale = hi or 1.0
        # y-axis: max at top, 0 at the baseline
        cr.set_source_rgb(0.5, 0.5, 0.5)
        cr.move_to(2, pad_t + 8); cr.show_text("%.0f ms" % hi)
        cr.move_to(2, pad_t + ph); cr.show_text("0")
        # baseline
        cr.set_source_rgb(0.2, 0.2, 0.2); cr.set_line_width(1.0)
        cr.move_to(pad_l, pad_t + ph); cr.line_to(pad_l + pw, pad_t + ph); cr.stroke()
        xs = lambda i: pad_l + pw * (i / (n - 1) if n > 1 else 0.5)
        ys = lambda v: pad_t + ph - (v / scale) * ph
        # polyline (0-based, so height == latency)
        cr.set_source_rgb(0.54, 0.89, 0.20); cr.set_line_width(1.5)
        for i, v in enumerate(r):
            (cr.line_to if i else cr.move_to)(xs(i), ys(v))
        cr.stroke()
        for i, v in enumerate(r):                  # sample dots
            cr.arc(xs(i), ys(v), 2.0, 0.0, 6.2832); cr.fill()
        # x caption
        cr.set_source_rgb(0.5, 0.5, 0.5)
        cr.move_to(pad_l, h - 2); cr.show_text("packet 1..%d  (left = first)" % n)

    def on_nat(self, _sw, active):
        wan = self._wan_iface()
        op = "-A" if active else "-D"
        rules = [
            ["iptables", "-t", "nat", op, "POSTROUTING", "-s", "172.16.7.0/24",
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
                self._set_status("NAT on via %s -- ping 8.8.8.8 from the board" % wan)
            else:
                for cmd in rules:
                    try:
                        self._run(cmd)
                    except Exception:
                        pass
                self.append("[nat] OFF\n")
                self._set_status("NAT off")
        except Exception as e:
            detail = ""
            err = getattr(e, "stderr", None)
            if err:
                detail = err.decode(errors="replace") if isinstance(err, bytes) else str(err)
            self.append("[nat] ERROR via %s: %s\n" % (wan, (detail or str(e)).strip()))
            self._set_status("NAT error -- see console", err=True)
        return False

    # ---- helpers ----------------------------------------------------------
    def _refresh_counters(self):
        self.cnt_lbl.set_text("frames in/out: %d/%d   bytes: %d/%d"
                              % (self.fr_in, self.fr_out, self.by_in, self.by_out))
        return GLib.SOURCE_CONTINUE

    def _set_status(self, text, err=False):
        self.status.set_text(("error: " if err and "error" not in text else "") + text)

    @staticmethod
    def _ip(args):
        SlmuxGui._run(["ip"] + args)

    @staticmethod
    def _run(args):
        import subprocess
        subprocess.run(args, check=True, capture_output=True)

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


def main():
    app = SlmuxGui()
    return app.run(None)


if __name__ == "__main__":
    sys.exit(main())
