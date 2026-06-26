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

        # --- WiFi (RP2350W): scan + connect over the console, no SLIP/sudo ---
        nbox.append(self._h("WiFi (RP2350W: scan + connect)"))
        wb = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        self.wifi_scan_btn = Gtk.Button(label="Scan")
        self.wifi_scan_btn.connect("clicked", self.on_wifi_scan)
        wb.append(self.wifi_scan_btn)
        self.wifi_status_lbl = Gtk.Label(); self.wifi_status_lbl.set_xalign(0)
        self.wifi_status_lbl.set_hexpand(True); self.wifi_status_lbl.set_wrap(True)
        self.wifi_status_lbl.set_markup("<span foreground='#888888'>idle</span>")
        wb.append(self.wifi_status_lbl)
        nbox.append(wb)
        wsw = Gtk.ScrolledWindow()
        wsw.set_min_content_height(110); wsw.set_max_content_height(170)
        wsw.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.wifi_list = Gtk.ListBox(); self.wifi_list.set_show_separators(True)
        self.wifi_list.connect("row-selected", self.on_wifi_row)
        self.wifi_list.add_css_class("console")
        wsw.set_child(self.wifi_list); nbox.append(wsw)
        se = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        se.append(Gtk.Label(label="SSID"))
        self.wifi_ssid = Gtk.Entry(); self.wifi_ssid.set_hexpand(True)
        self.wifi_ssid.connect("activate", self.on_wifi_connect); se.append(self.wifi_ssid)
        nbox.append(se)
        pe = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        pe.append(Gtk.Label(label="Pass"))
        self.wifi_pwd = Gtk.PasswordEntry(); self.wifi_pwd.set_show_peek_icon(True)
        self.wifi_pwd.set_hexpand(True)
        self.wifi_pwd.connect("activate", self.on_wifi_connect); pe.append(self.wifi_pwd)
        nbox.append(pe)
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
        nbox.append(cb)
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
