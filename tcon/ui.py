"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.ui - UiMixin: the networking side-pane and small UI helpers.

Builds the collapsible "Networking" panel (SLIP toggle, NAT, ping controls +
animation + RTT chart + output) and the shared helpers: bold section headers,
the byte/frame counter tick, and the status line.

SPDX-License-Identifier: Apache-2.0
"""
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib, Pango  # noqa: E402

from tcon import GREEN, BOARD_IP  # noqa: E402


class UiMixin:
    def _build_netpanel(self):
        nbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        nbox.set_size_request(340, -1)
        self.wifi_pane = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)

        # --- WiFi (RP2350W): scan + connect + go online, over the console ---
        self.wifi_pane.append(self._h("WiFi (RP2350W: scan · connect · internet)"))
        wb = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        self.wifi_scan_btn = Gtk.Button(label="Scan")
        self.wifi_scan_btn.connect("clicked", self.on_wifi_scan)
        wb.append(self.wifi_scan_btn)
        self.wifi_status_lbl = Gtk.Label(); self.wifi_status_lbl.set_xalign(0)
        self.wifi_status_lbl.set_hexpand(True); self.wifi_status_lbl.set_wrap(True)
        self.wifi_status_lbl.set_markup("<span foreground='#888888'>idle</span>")
        wb.append(self.wifi_status_lbl)
        self.wifi_pane.append(wb)
        wsw = Gtk.ScrolledWindow()
        wsw.set_min_content_height(110); wsw.set_max_content_height(170)
        wsw.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.wifi_list = Gtk.ListBox(); self.wifi_list.set_show_separators(True)
        self.wifi_list.connect("row-selected", self.on_wifi_row)
        self.wifi_list.add_css_class("console")
        wsw.set_child(self.wifi_list); self.wifi_pane.append(wsw)
        se = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        se.append(Gtk.Label(label="SSID"))
        self.wifi_ssid = Gtk.Entry(); self.wifi_ssid.set_hexpand(True)
        self.wifi_ssid.connect("activate", self.on_wifi_connect); se.append(self.wifi_ssid)
        self.wifi_pane.append(se)
        pe = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        pe.append(Gtk.Label(label="Pass"))
        self.wifi_pwd = Gtk.PasswordEntry(); self.wifi_pwd.set_show_peek_icon(True)
        self.wifi_pwd.set_hexpand(True)
        self.wifi_pwd.connect("activate", self.on_wifi_connect); pe.append(self.wifi_pwd)
        self.wifi_pane.append(pe)
        cb = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        self.wifi_wpa3 = Gtk.CheckButton(label="WPA3")
        self.wifi_wpa3.set_tooltip_text("Use WPA3-SAE (connect3) instead of WPA2-PSK")
        cb.append(self.wifi_wpa3)
        self.wifi_disc_btn = Gtk.Button(label="Disconnect")
        self.wifi_disc_btn.connect("clicked", self.on_wifi_disconnect)
        cb.append(self.wifi_disc_btn)
        self.wifi_conn_btn = Gtk.Button(label="Connect")
        self.wifi_conn_btn.add_css_class("suggested-action")
        self.wifi_conn_btn.set_hexpand(True); self.wifi_conn_btn.set_halign(Gtk.Align.END)
        self.wifi_conn_btn.connect("clicked", self.on_wifi_connect)
        cb.append(self.wifi_conn_btn)
        self.wifi_pane.append(cb)

        # --- On the network: DHCP lease + internet checks, all over WiFi ---
        # Connect already joins AND brings the IP up; these let you re-run the
        # DHCP, and ping / fetch time straight from the board over its radio.
        self.wifi_ip_lbl = Gtk.Label(); self.wifi_ip_lbl.set_xalign(0)
        self.wifi_ip_lbl.set_wrap(True); self.wifi_ip_lbl.set_selectable(True)
        self.wifi_ip_lbl.set_markup(
            "<span foreground='#888888'>not on the network — Connect joins "
            "and brings the IP up</span>")
        self.wifi_pane.append(self.wifi_ip_lbl)
        ab = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        self.wifi_up_btn = Gtk.Button(label="Go online")
        self.wifi_up_btn.set_tooltip_text("Bring the board's IP stack up over "
                                          "WiFi (runs 'wifi up' → DHCP)")
        self.wifi_up_btn.connect("clicked", self.on_wifi_online)
        ab.append(self.wifi_up_btn)
        self.wifi_ping_btn = Gtk.Button(label="Ping")
        self.wifi_ping_btn.set_tooltip_text("The board pings this host over its "
                                            "own WiFi (default 8.8.8.8)")
        self.wifi_ping_btn.connect("clicked", self.on_wifi_ping_net)
        ab.append(self.wifi_ping_btn)
        self.wifi_ping_t = Gtk.Entry(text="8.8.8.8")
        self.wifi_ping_t.set_hexpand(True); self.wifi_ping_t.set_max_width_chars(14)
        self.wifi_ping_t.connect("activate", self.on_wifi_ping_net)
        ab.append(self.wifi_ping_t)
        self.wifi_ntp_btn = Gtk.Button(label="Time")
        self.wifi_ntp_btn.set_tooltip_text("Fetch UTC time from an internet NTP "
                                           "server, over WiFi")
        self.wifi_ntp_btn.connect("clicked", self.on_wifi_ntp)
        ab.append(self.wifi_ntp_btn)
        self.wifi_pane.append(ab)
        self.wifi_net_lbl = Gtk.Label(); self.wifi_net_lbl.set_xalign(0)
        self.wifi_net_lbl.set_wrap(True); self.wifi_net_lbl.set_selectable(True)
        self.wifi_pane.append(self.wifi_net_lbl)
        nbox.append(self.wifi_pane)
        self.wifi_pane.set_visible(False)   # hidden until a RP2350 port is identified

        nbox.append(Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL))

        nbox.append(self._h("Networking (SLIP/IP over the wire)"))
        self.net_hint = Gtk.Label(); self.net_hint.set_xalign(0)
        self.net_hint.set_wrap(True); self.net_hint.set_visible(False)
        nbox.append(self.net_hint)
        self.slip_btn = Gtk.Button(label="Toggle SLIP on board")
        self.slip_btn.set_sensitive(False)
        self.slip_btn.connect("clicked", lambda b: self.send_line("slip"))
        nbox.append(self.slip_btn)
        self.tun_lbl = Gtk.Label(label="tun0: down"); self.tun_lbl.set_xalign(0)
        nbox.append(self.tun_lbl)
        self.cnt_lbl = Gtk.Label(label="frames in/out: 0/0   bytes: 0/0")
        self.cnt_lbl.set_xalign(0); nbox.append(self.cnt_lbl)
        # SLIP activity LEDs -- the arrows pulse + glow as frames cross the wire
        # (cyan = host->board, green = board->host); driven by the gateway via
        # _slip_blink() at the serial<->tun forwarding points.  Watch them
        # shimmer during a BASIC BROWSE / HTTPGET$.
        sa = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        sa.append(Gtk.Label(label="SLIP"))
        self.slip_tx_lbl = Gtk.Label(label="▲ to board")
        self.slip_tx_lbl.add_css_class("slip-led")
        self.slip_tx_lbl.set_tooltip_text("host → board: a SLIP frame was sent")
        self.slip_rx_lbl = Gtk.Label(label="▼ from board")
        self.slip_rx_lbl.add_css_class("slip-led")
        self.slip_rx_lbl.set_tooltip_text("board → host: a SLIP frame arrived")
        sa.append(self.slip_tx_lbl); sa.append(self.slip_rx_lbl)
        nbox.append(sa)

        nbox.append(self._h("Internet (NAT: board → internet)"))
        natb = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        natb.append(Gtk.Label(label="enable"))
        self.nat = Gtk.Switch(); self.nat.set_valign(Gtk.Align.CENTER)
        self.nat.set_sensitive(False)
        self.nat.connect("state-set", self.on_nat); natb.append(self.nat)
        nbox.append(natb)

        nbox.append(self._h("Ping (host kernel → via tun0)"))
        pb = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        self.ping_t = Gtk.Entry(text=BOARD_IP); self.ping_t.set_hexpand(True)
        self.ping_t.connect("activate", self.on_ping); pb.append(self.ping_t)
        pb.append(Gtk.Label(label="×"))
        self.ping_n_spin = Gtk.SpinButton.new_with_range(1, 100, 1)
        self.ping_n_spin.set_value(5)
        self.ping_n_spin.set_tooltip_text("number of ping packets to send")
        pb.append(self.ping_n_spin)
        self.ping_btn = Gtk.Button(label="Ping")
        self.ping_btn.connect("clicked", self.on_ping); pb.append(self.ping_btn)
        nbox.append(pb)
        self.ping_anim = Gtk.DrawingArea(); self.ping_anim.set_content_height(66)
        self.ping_anim.set_draw_func(self._draw_ping_anim)
        self.ping_anim.set_tooltip_text("this PC ──packets──> board (animated "
                                        "while pinging)")
        nbox.append(self.ping_anim)
        self.ping_stats = Gtk.Label(label="idle -- enter an address and click Ping")
        self.ping_stats.set_xalign(0); self.ping_stats.set_selectable(True)
        self.ping_stats.set_wrap(True); nbox.append(self.ping_stats)
        cap = Gtk.Label(label="round-trip time per packet (taller = slower):")
        cap.set_xalign(0); cap.add_css_class("dim-label"); nbox.append(cap)
        self.spark = Gtk.DrawingArea(); self.spark.set_content_height(74)
        self.spark.set_draw_func(self._draw_spark); nbox.append(self.spark)
        psw = Gtk.ScrolledWindow(); psw.set_min_content_height(120)
        psw.set_vexpand(True)
        self.ping_view = Gtk.TextView(); self.ping_view.set_editable(False)
        self.ping_view.set_monospace(True); self.ping_view.add_css_class("console")
        self.ping_buf = self.ping_view.get_buffer()
        self.ping_ok = self.ping_buf.create_tag("ok", foreground=GREEN)
        self.ping_bad = self.ping_buf.create_tag("bad", foreground="#ff6b6b")
        psw.set_child(self.ping_view); nbox.append(psw)
        return nbox

    def set_wifi_pane_visible(self, plat):
        """Show the WiFi pane only for RP2350-class boards (the only ones with
        onboard WiFi); Apollo/MSP430 use SLIP, so hide it there.  Also records
        self._wifi_board so the on-connect `wifi status` auto-probe (_wifi_sync)
        stays silent on boards that have no `wifi` command."""
        p = (plat or "").lower()
        self._wifi_board = ("rp2" in p or "pico" in p)
        if hasattr(self, "wifi_pane"):
            self.wifi_pane.set_visible(self._wifi_board)

    def _h(self, text):
        lbl = Gtk.Label(label=text); lbl.set_xalign(0)
        lbl.set_attributes(self._bold()); lbl.set_margin_top(6)
        return lbl

    @staticmethod
    def _bold():
        a = Pango.AttrList(); a.insert(Pango.attr_weight_new(Pango.Weight.BOLD))
        return a

    def _refresh_counters(self):
        self.cnt_lbl.set_text("frames in/out: %d/%d   bytes: %d/%d"
                              % (self.fr_in, self.fr_out, self.by_in, self.by_out))
        return GLib.SOURCE_CONTINUE

    def _set_status(self, text, err=False):
        self.status.set_text(("error: " if err and "error" not in text else "")
                             + text)
