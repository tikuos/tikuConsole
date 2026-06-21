"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.ping - PingMixin: rootless ICMP-over-SLIP board pinger.

Crafts ICMP echo requests in userspace, SLIPs them to the board (whose own
stack answers), matches the replies in _on_ip_packet, and draws the RTT chart,
the ping(8)-style stats, and the animated this-PC -> board illustration.
No TUN, no system 'ping', no root.

SPDX-License-Identifier: Apache-2.0
"""
import time
import socket
from gi.repository import GLib

from slmux import slip_encode
from tcon import HOST_IP
from tcon.packets import build_ip, build_icmp_echo, parse_icmp_echo_reply


class PingMixin:
    def on_ping(self, _w):
        if self.ping_active:                        # one run at a time
            return
        if self.ser is None:
            self.ping_stats.set_text("connect first"); return
        target = self.ping_t.get_text().strip()
        if target:
            self._slip_ping(target)

    def _slip_ping(self, target):
        """Rootless ping: craft ICMP echo requests, SLIP them to the board, and
        match the replies in _on_ip_packet.  No TUN, no system 'ping', no root
        -- the board's own ICMP stack answers echo requests."""
        self.ping_active = True
        self.ping_rtts = []; self.ping_sent = 0; self.ping_recv = 0
        self.ping_seq_t = {}
        self.ping_target = target
        self.ping_ident = (self.ping_ident + 1) & 0xffff
        self.ping_i = 0; self.ping_n = int(self.ping_n_spin.get_value())
        self.ping_buf.set_text(""); self.spark.queue_draw()
        self.ping_stats.set_text("pinging %s over SLIP ..." % target)
        self.ping_btn.set_sensitive(False)
        self.ping_anim_pkts = []
        if not self.ping_anim_src:                  # drive the packet animation
            self.ping_anim_src = GLib.timeout_add(50, self._ping_anim_tick)
        self.send_line("slip on")                  # ensure board SLIP (idempotent)
        GLib.timeout_add(350, self._slip_ping_tick)  # settle, then one probe/tick

    def _slip_ping_tick(self):
        if not self.ping_active:                    # cancelled (disconnect / off)
            return GLib.SOURCE_REMOVE
        if self.ping_i >= self.ping_n:             # all sent -> wait, then finish
            GLib.timeout_add(1000, self._slip_ping_finish)
            return GLib.SOURCE_REMOVE
        seq = self.ping_i; self.ping_i += 1
        try:
            dst = socket.inet_aton(self.ping_target)
        except OSError:
            self.ping_stats.set_text("bad address: %s" % self.ping_target)
            self.ping_active = False; self.ping_btn.set_sensitive(True)
            return GLib.SOURCE_REMOVE
        pkt = build_ip(socket.inet_aton(HOST_IP), dst, 1,
                       build_icmp_echo(self.ping_ident, seq, b"tikuconsole"))
        self.ping_seq_t[seq] = time.monotonic()
        try:
            self.ser.write(slip_encode(pkt))
        except Exception:
            self.ping_active = False; self.ping_btn.set_sensitive(True)
            return GLib.SOURCE_REMOVE
        self.ping_sent += 1; self.fr_out += 1; self.by_out += len(pkt)
        self.ping_anim_pkts.append(0.0)             # one moving icon per probe
        return GLib.SOURCE_CONTINUE                 # next probe next tick

    def _ping_rx(self, pkt):
        seq = parse_icmp_echo_reply(pkt, self.ping_ident)
        if seq is None:
            return
        t0 = self.ping_seq_t.pop(seq, None)
        if t0 is None:                             # dup or already-finished
            return
        rtt = (time.monotonic() - t0) * 1000.0
        self.ping_rtts.append(rtt); self.ping_recv += 1
        self.ping_pulse = 8                         # flash the board glyph
        bar = "█" * max(1, int(round(20 * rtt / (max(self.ping_rtts) or 1))))
        self._ping_row("packet %-3d %7.1f ms  %s" % (seq, rtt, bar), self.ping_ok)
        self._ping_stats(); self.spark.queue_draw()

    def _slip_ping_finish(self):
        if not self.ping_active:
            return GLib.SOURCE_REMOVE
        self.ping_active = False
        self.ping_btn.set_sensitive(True)
        for seq in sorted(self.ping_seq_t):        # never answered
            self._ping_row("packet %-3d  no reply (timed out)" % seq,
                           self.ping_bad)
        self.ping_seq_t = {}
        # --- statistics block (ping(8)-style), appended to the output ---
        r = self.ping_rtts
        sent, recv = self.ping_sent, self.ping_recv
        loss = int(round(100 * (sent - recv) / sent)) if sent else 0
        tag = self.ping_ok if recv else self.ping_bad
        self._ping_row("--- %s ping statistics ---" % self.ping_target, tag)
        self._ping_row("%d packets sent, %d received, %d%% packet loss"
                       % (sent, recv, loss), tag)
        if r:
            self._ping_row("rtt  min %.1f / avg %.1f / max %.1f ms"
                           % (min(r), sum(r) / len(r), max(r)), tag)
        self._ping_stats(); self.spark.queue_draw()
        return GLib.SOURCE_REMOVE

    def _ping_row(self, text, tag):
        self.ping_buf.insert_with_tags(self.ping_buf.get_end_iter(), text + "\n",
                                       tag)
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

    def _ping_anim_tick(self):
        # advance each in-flight packet toward the board; drop those that arrive
        self.ping_anim_pkts = [p + 0.033 for p in self.ping_anim_pkts
                               if p + 0.033 < 1.0]
        if self.ping_pulse > 0:
            self.ping_pulse -= 1
        self.ping_anim.queue_draw()
        if (not self.ping_active and self.ping_pulse <= 0
                and not self.ping_anim_pkts):
            self.ping_anim_src = 0                  # idle -> stop animating
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE

    def _draw_ping_anim(self, area, cr, w, h, *a):
        """Illustrate this PC -> board over the wire: a monitor on the left, the
        controller chip on the right, IPs underneath, and amber packets sliding
        across while a ping runs (the chip glows green on each reply)."""
        cr.set_source_rgb(0.043, 0.043, 0.043); cr.paint()
        midy = h * 0.46
        pcx, bdx = 28.0, w - 28.0
        cr.set_source_rgb(0.33, 0.33, 0.33); cr.set_line_width(1.5)   # wire
        cr.move_to(pcx + 16, midy); cr.line_to(bdx - 14, midy); cr.stroke()
        # this PC: a little monitor + stand
        cr.set_source_rgb(0.16, 0.50, 0.82); cr.set_line_width(1.8)
        cr.rectangle(pcx - 14, midy - 10, 27, 16); cr.stroke()
        cr.move_to(pcx - 3, midy + 6); cr.line_to(pcx - 6, midy + 11)
        cr.move_to(pcx + 2, midy + 6); cr.line_to(pcx + 5, midy + 11)
        cr.move_to(pcx - 8, midy + 11); cr.line_to(pcx + 7, midy + 11); cr.stroke()
        # board / controller: a chip with pins (glows on a reply)
        if self.ping_pulse > 0:
            cr.set_source_rgba(0.30, 0.80, 0.42, 0.45)
            cr.arc(bdx, midy, 18, 0, 6.2832); cr.fill()
        cr.set_source_rgb(0.20, 0.66, 0.36); cr.set_line_width(1.8)
        cr.rectangle(bdx - 9, midy - 9, 18, 18); cr.stroke()
        for i in range(3):
            yy = midy - 5 + i * 5
            cr.move_to(bdx - 9, yy); cr.line_to(bdx - 13, yy)
            cr.move_to(bdx + 9, yy); cr.line_to(bdx + 13, yy)
        cr.stroke()
        # one packet icon (envelope) per ping actually sent, sliding PC -> board
        x0, x1 = pcx + 18, bdx - 16
        for prog in self.ping_anim_pkts:
            x = x0 + (x1 - x0) * prog
            cr.set_source_rgb(0.96, 0.70, 0.14)          # amber envelope body
            cr.rectangle(x - 5.5, midy - 3.5, 11, 7); cr.fill()
            cr.set_source_rgb(0.22, 0.15, 0.03)          # dark outline + flap
            cr.set_line_width(0.9)
            cr.rectangle(x - 5.5, midy - 3.5, 11, 7); cr.stroke()
            cr.move_to(x - 5.5, midy - 3.5)
            cr.line_to(x, midy + 0.5)
            cr.line_to(x + 5.5, midy - 3.5); cr.stroke()
        # labels: roles above, IPs below
        cr.select_font_face("monospace")

        def ctext(x, y, s, size, g):
            cr.set_source_rgb(g, g, g); cr.set_font_size(size)
            e = cr.text_extents(s); cr.move_to(x - e.width / 2.0, y); cr.show_text(s)
        ctext(pcx, midy - 15, "this PC", 8, 0.5)
        ctext(bdx, midy - 15, "board", 8, 0.5)
        ctext(pcx, h - 3, HOST_IP, 9, 0.66)
        ctext(bdx, h - 3, self.ping_target, 9, 0.66)

    def _draw_spark(self, area, cr, w, h, *a):
        cr.set_source_rgb(0.04, 0.04, 0.04); cr.paint()
        cr.select_font_face("monospace"); cr.set_font_size(10)
        r = self.ping_rtts
        if not r:
            cr.set_source_rgb(0.45, 0.45, 0.45)
            msg = "no data yet -- click Ping to chart round-trip time"
            ext = cr.text_extents(msg)
            cr.move_to((w - ext.width) / 2.0, h / 2.0 + 4); cr.show_text(msg)
            return
        pad_l, pad_t, pad_b = 38.0, 4.0, 13.0      # axis margins
        pw, ph = max(1.0, w - pad_l - 6), max(1.0, h - pad_t - pad_b)
        hi = max(r); n = len(r); scale = hi or 1.0
        cr.set_source_rgb(0.5, 0.5, 0.5)           # y-axis: max top, 0 baseline
        cr.move_to(2, pad_t + 8); cr.show_text("%.0f ms" % hi)
        cr.move_to(2, pad_t + ph); cr.show_text("0")
        cr.set_source_rgb(0.2, 0.2, 0.2); cr.set_line_width(1.0)
        cr.move_to(pad_l, pad_t + ph); cr.line_to(pad_l + pw, pad_t + ph); cr.stroke()
        xs = lambda i: pad_l + pw * (i / (n - 1) if n > 1 else 0.5)
        ys = lambda v: pad_t + ph - (v / scale) * ph
        cr.set_source_rgb(0.54, 0.89, 0.20); cr.set_line_width(1.5)
        for i, v in enumerate(r):
            (cr.line_to if i else cr.move_to)(xs(i), ys(v))
        cr.stroke()
        for i, v in enumerate(r):                  # sample dots
            cr.arc(xs(i), ys(v), 2.0, 0.0, 6.2832); cr.fill()
        cr.set_source_rgb(0.5, 0.5, 0.5)
        cr.move_to(pad_l, h - 2); cr.show_text("packet 1..%d  (left = first)" % n)
