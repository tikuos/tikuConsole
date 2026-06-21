"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.splash - SplashMixin: the startup splash screen.

A TikuBench-style splash (brand accent strip, logo, title, license and a brief
loading bar) shown for ~2 s, then it presents the main window and destroys
itself.  Logos load from tcon.LOGO_DIR; a bold 'TikuOS' label stands in if
they are missing.

SPDX-License-Identifier: Apache-2.0
"""
import os
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib  # noqa: E402

from tcon import VERSION, LOGO_DIR  # noqa: E402


class SplashMixin:
    def _show_splash(self):
        sp = Gtk.ApplicationWindow(application=self)
        sp.set_decorated(False); sp.set_resizable(False)
        sp.set_default_size(460, 520); sp.add_css_class("splash")
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        sp.set_child(outer)
        accent = Gtk.Box(); accent.add_css_class("splash-accent")
        accent.set_size_request(-1, 7); outer.append(accent)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        box.add_css_class("splash-body")
        box.set_valign(Gtk.Align.CENTER); box.set_halign(Gtk.Align.CENTER)
        box.set_vexpand(True); outer.append(box)
        tlogo = os.path.join(LOGO_DIR, "tikuos.jpeg")
        if os.path.exists(tlogo):
            pic = Gtk.Picture.new_for_filename(tlogo)
            pic.set_content_fit(Gtk.ContentFit.CONTAIN)
            pic.set_size_request(140, 140); pic.set_halign(Gtk.Align.CENTER)
            box.append(pic)
        else:
            art = Gtk.Label()
            art.set_markup("<span size='xx-large' weight='bold' "
                           "foreground='#14457f'>TikuOS</span>")
            box.append(art)
        title = Gtk.Label(label="TikuConsole"); title.add_css_class("splash-title")
        box.append(title)
        sub = Gtk.Label(); sub.add_css_class("splash-sub")
        sub.set_markup("serial console for "
                       "<span foreground='#2e7d32'><b>TikuOS</b></span> devices"
                       "  ·  v%s" % VERSION)
        box.append(sub)
        box.append(Gtk.Label(label=""))
        au = Gtk.Label(label="Ambuj Varshney"); au.add_css_class("splash-author")
        box.append(au)
        lic = Gtk.Label(label="© TikuOS · Licensed under Apache-2.0")
        lic.add_css_class("splash-lic"); box.append(lic)
        box.append(Gtk.Label(label=""))
        lrow = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        lrow.set_halign(Gtk.Align.CENTER)
        spin = Gtk.Spinner(); spin.start(); lrow.append(spin)
        self._splash_load = Gtk.Label(label="Loading")
        self._splash_load.add_css_class("splash-load"); lrow.append(self._splash_load)
        box.append(lrow)
        self._splash_prog = Gtk.ProgressBar()
        self._splash_prog.set_size_request(300, -1)
        self._splash_prog.set_margin_top(4); box.append(self._splash_prog)
        wlogo = os.path.join(LOGO_DIR, "weiser.png")
        if os.path.exists(wlogo):
            wp = Gtk.Picture.new_for_filename(wlogo)
            wp.set_content_fit(Gtk.ContentFit.CONTAIN)
            wp.set_size_request(170, 96); wp.set_halign(Gtk.Align.CENTER)
            wp.set_margin_top(10); box.append(wp)
        self._splash = sp; self._splash_ticks = 0
        sp.present()
        GLib.timeout_add(140, self._splash_tick)

    def _splash_tick(self):
        self._splash_ticks += 1
        self._splash_load.set_text("Loading" + "." * (self._splash_ticks & 3))
        self._splash_prog.set_fraction(min(1.0, self._splash_ticks / 16.0))
        if self._splash_ticks >= 16:
            self.win.present(); self.cview.grab_focus()
            self._splash.destroy()
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE
