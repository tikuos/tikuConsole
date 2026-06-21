"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.app - the TikuConsole Gtk.Application.

Holds the session state, builds the main window (do_activate), and composes the
per-subsystem mixins into one application object.  Each subsystem (console,
connection, ping, nat, leds, splash, ui) lives in its own module; they share
one instance, so methods stay plain `self.*` calls.

SPDX-License-Identifier: Apache-2.0
"""
import os
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gdk, GLib  # noqa: E402

from tcon import GREEN, VERSION, BOARD_IP, LOGO_DIR  # noqa: E402
from tcon.console import ConsoleMixin  # noqa: E402
from tcon.connection import ConnectionMixin  # noqa: E402
from tcon.ping import PingMixin  # noqa: E402
from tcon.nat import NatMixin  # noqa: E402
from tcon.leds import LedsMixin  # noqa: E402
from tcon.splash import SplashMixin  # noqa: E402
from tcon.ui import UiMixin  # noqa: E402


class TikuConsole(ConsoleMixin, ConnectionMixin, PingMixin, NatMixin,
                  LedsMixin, SplashMixin, UiMixin, Gtk.Application):
    def __init__(self):
        super().__init__(application_id="org.tikuos.tikuconsole")
        self.ser = None
        self.tun = -1
        self.ser_src = 0
        self.tun_src = 0
        self.net = False                 # networking mode active this session
        self.ports = []
        self.port_path = None
        # SLIP demux state
        self.in_frame = False
        self.frame = bytearray()
        # traffic-light status: board SLIP enabled / host NAT (Internet) active
        self.slip_on = False
        self.nat_on = False
        self.slip_scan = ""              # rolling console tail for SLIP detection
        # counters
        self.fr_in = self.fr_out = self.by_in = self.by_out = 0
        # in-app ICMP-over-SLIP ping (rootless: no TUN / no system 'ping')
        self.ping_active = False
        self.ping_ident = 0x4242
        self.ping_seq_t = {}                  # seq -> send time (monotonic)
        self.ping_rtts = []
        self.ping_sent = self.ping_recv = 0
        self.ping_target = BOARD_IP           # last target IP (for the animation)
        self.ping_anim_pkts = []              # in-flight packet icons: progress 0..1
        self.ping_anim_src = 0
        self.ping_pulse = 0                   # board glyph glows briefly on reply

    # ---- UI ---------------------------------------------------------------
    def do_activate(self):
        win = Gtk.ApplicationWindow(application=self, title="TikuConsole")
        win.set_default_size(960, 600)
        win.set_titlebar(Gtk.HeaderBar())          # window controls incl. maximize
        self.win = win
        try:                                       # window / taskbar icon
            theme = Gtk.IconTheme.get_for_display(Gdk.Display.get_default())
            theme.add_search_path(LOGO_DIR)
            win.set_icon_name("org.tikuos.tikuconsole")
        except Exception:
            pass

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        for m in ("top", "bottom", "start", "end"):
            getattr(root, "set_margin_" + m)(8)
        win.set_child(root)

        # --- banner row: title (left) + status lights (right) ---
        brow = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        banner = Gtk.Label(); banner.set_xalign(0); banner.set_hexpand(True)
        banner.set_markup(
            "<span size='xx-large' weight='bold' foreground='%s'>TikuConsole"
            "</span>  <span size='small' foreground='#888888'>v%s</span>\n"
            "<span size='small' foreground='#888888'>serial console for "
            "TikuOS devices  ·  networking optional</span>"
            % (GREEN, VERSION))
        brow.append(banner)
        leds = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=14)
        leds.set_valign(Gtk.Align.CENTER); leds.set_halign(Gtk.Align.END)
        self.usb_led = Gtk.Label(); self.slip_led = Gtk.Label()
        self.nat_led = Gtk.Label()
        for _l in (self.usb_led, self.slip_led, self.nat_led):
            leds.append(_l)
        brow.append(leds)
        root.append(brow)

        # --- connection bar ---
        bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        bar.append(Gtk.Label(label="Port"))
        self.port_dd = Gtk.DropDown(model=Gtk.StringList.new([""]))
        self.port_dd.connect("notify::selected", self.on_port_changed)
        bar.append(self.port_dd)
        refresh = Gtk.Button(label="⟳"); refresh.set_tooltip_text("Rescan ports")
        refresh.connect("clicked", self.refresh_ports); bar.append(refresh)
        self.platform_lbl = Gtk.Label(label="--")
        self.platform_lbl.add_css_class("dim-label"); bar.append(self.platform_lbl)
        bar.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL))
        bar.append(Gtk.Label(label="Baud"))
        self.baud = Gtk.Entry(text="115200"); self.baud.set_max_width_chars(7)
        bar.append(self.baud)
        bar.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL))
        bar.append(Gtk.Label(label="Networking"))
        self.net_sw = Gtk.Switch(); self.net_sw.set_valign(Gtk.Align.CENTER)
        self.net_sw.set_tooltip_text("Bring up SLIP/IP + TUN over the same wire "
                                     "(needs sudo)")
        self.net_sw.connect("state-set", self.on_net_toggle); bar.append(self.net_sw)
        self.connect_btn = Gtk.Button(label="Connect")
        self.connect_btn.add_css_class("suggested-action")
        self.connect_btn.connect("clicked", self.on_connect)
        self.connect_btn.set_hexpand(True); self.connect_btn.set_halign(Gtk.Align.END)
        bar.append(self.connect_btn)
        root.append(bar)

        self.status = Gtk.Label(label="disconnected"); self.status.set_xalign(0)
        self.status.set_selectable(True); self.status.set_wrap(True)
        root.append(self.status)

        # --- main row: console | (optional) network panel, draggable divider ---
        mainrow = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        mainrow.set_vexpand(True); mainrow.set_wide_handle(True)
        root.append(mainrow)

        cbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        cbox.set_hexpand(True)
        sw = Gtk.ScrolledWindow(); sw.set_vexpand(True)
        self.cview = Gtk.TextView(); self.cview.set_editable(False)
        self.cview.set_monospace(True); self.cview.set_wrap_mode(Gtk.WrapMode.CHAR)
        self.cbuf = self.cview.get_buffer(); sw.set_child(self.cview)
        self.cview.add_css_class("console")
        self.cview.add_css_class("console-off")    # greyed until connected
        self._init_console_style()
        # Type straight into the console from anywhere in the window: a
        # capture-phase key controller forwards keystrokes to the board whenever
        # connected, *unless* a text field (baud/ping/count) holds focus.  Being
        # focus-independent, typing works the instant you (re)connect.
        kc = Gtk.EventControllerKey()
        kc.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        kc.connect("key-pressed", self._route_key)
        win.add_controller(kc)
        self.cview.set_can_focus(True)
        cbox.append(sw)
        mainrow.set_start_child(cbox)
        mainrow.set_resize_start_child(True)        # console grows with the window
        mainrow.set_shrink_start_child(False)

        self.netpanel = self._build_netpanel()
        self.netpanel.set_visible(False)           # shown only in Networking mode
        mainrow.set_end_child(self.netpanel)
        mainrow.set_resize_end_child(False)         # pane keeps its width on resize
        mainrow.set_shrink_end_child(False)
        mainrow.set_position(600)                   # initial split; drag to taste

        GLib.timeout_add(500, self._refresh_counters)
        self.refresh_ports()
        self._update_leds()                        # initial light state (all off)

        smoke = os.environ.get("TIKUCONSOLE_SMOKE_MS")
        if smoke or os.environ.get("TIKUCONSOLE_NO_SPLASH"):
            win.present(); self.cview.grab_focus()  # straight to the main window
        else:
            self._show_splash()                     # presents win when it finishes
        if smoke:
            GLib.timeout_add(int(smoke), lambda: (self.quit(), False)[1])


def main():
    return TikuConsole().run(None)
