"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.wifi - WiFiMixin: scan + connect a board's WiFi from the console.

Orchestrates the board's `wifi` shell command (RP2350W / CYW43439) over the
same serial line the console uses -- no extra transport.  "Scan" runs
`wifi scan` then `wifi list` and parses the table into a network list; pick a
row (or type an SSID), enter the passphrase, and "Connect" issues
`wifi connect`/`connect3` and polls `wifi status` until the link comes up.

Replies are captured by tapping the incoming console text (_wifi_feed, called
from ConnectionMixin._on_serial) only while a scan/status query is in flight,
so the live console is never disturbed.

SPDX-License-Identifier: Apache-2.0
"""
import re
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib  # noqa: E402

from tcon import GREEN  # noqa: E402

# Strip SGR colour codes before matching, and parse one `wifi list` row:
#   " 1  60:e3:27:4b:7a:61   -91   1  KV2-SPARE-079"
_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_AP_RE = re.compile(r"^\s*\d+\s+([0-9a-fA-F:]{17})\s+(-?\d+)\s+(\d+)\s+(.+?)\s*$")


class WiFiMixin:
    def _wifi_init(self):
        """Called from TikuConsole.__init__ (state only; widgets live in ui.py)."""
        self._wifi_linebuf = ""
        self._wifi_capture = None      # None | "list" | "status"
        self._wifi_aps = []
        self._wifi_status = {}
        self._wifi_poll = 0

    # ---- incoming console line feed (tapped by ConnectionMixin._on_serial) ----
    def _wifi_feed(self, text):
        """Accumulate console bytes into lines while a query is in flight."""
        if self._wifi_capture is None:
            return
        self._wifi_linebuf += text
        while "\n" in self._wifi_linebuf:
            line, self._wifi_linebuf = self._wifi_linebuf.split("\n", 1)
            self._wifi_line(_ANSI.sub("", line).rstrip("\r"))

    def _wifi_line(self, line):
        if self._wifi_capture == "list":
            m = _AP_RE.match(line)
            if m:
                bssid, rssi, ch, ssid = m.groups()
                self._wifi_aps.append({"ssid": ssid, "rssi": int(rssi),
                                       "ch": int(ch), "bssid": bssid})
        elif self._wifi_capture == "status":
            for key in ("State", "Link", "RSSI", "MAC"):
                if line.startswith(key + ":"):
                    self._wifi_status[key.lower()] = line.split(":", 1)[1].strip()

    # ---- Scan -------------------------------------------------------------
    def on_wifi_scan(self, _b=None):
        if self.ser is None:
            self._wifi_say("connect to a board first"); return
        self.wifi_scan_btn.set_sensitive(False)
        self._wifi_say("scanning…")
        self.send_line("wifi scan")
        GLib.timeout_add(2500, self._wifi_do_list)     # scan ~0.7s + margin

    def _wifi_do_list(self):
        if self.ser is None:
            self.wifi_scan_btn.set_sensitive(True); return False
        self._wifi_aps = []
        self._wifi_linebuf = ""
        self._wifi_capture = "list"
        self.send_line("wifi list")
        GLib.timeout_add(900, self._wifi_show_aps)     # let the table print
        return False

    def _wifi_show_aps(self):
        self._wifi_capture = None
        self.wifi_scan_btn.set_sensitive(True)
        # de-dup by BSSID (the table already is), keep strongest RSSI per SSID
        best = {}
        for ap in self._wifi_aps:
            cur = best.get(ap["ssid"])
            if cur is None or ap["rssi"] > cur["rssi"]:
                best[ap["ssid"]] = ap
        aps = sorted(best.values(), key=lambda a: -a["rssi"])
        self._wifi_populate(aps)
        self._wifi_say("%d network%s found" % (len(aps), "" if len(aps) == 1 else "s"))
        return False

    def _wifi_populate(self, aps):
        child = self.wifi_list.get_first_child()
        while child is not None:
            nxt = child.get_next_sibling()
            self.wifi_list.remove(child)
            child = nxt
        for ap in aps:
            row = Gtk.ListBoxRow()
            lbl = Gtk.Label(xalign=0)
            lbl.set_markup(
                "%s   <span foreground='#888888' size='small'>"
                "%s  ch%d  %ddBm</span>"
                % (GLib.markup_escape_text(ap["ssid"]),
                   self._wifi_bars(ap["rssi"]), ap["ch"], ap["rssi"]))
            row.set_child(lbl)
            row._ssid = ap["ssid"]
            self.wifi_list.append(row)

    def on_wifi_row(self, _lb, row):
        if row is not None and hasattr(row, "_ssid"):
            self.wifi_ssid.set_text(row._ssid)

    # ---- Connect ----------------------------------------------------------
    def on_wifi_connect(self, _b=None):
        if self.ser is None:
            self._wifi_say("connect to a board first"); return
        ssid = self.wifi_ssid.get_text().strip()
        psk = self.wifi_pwd.get_text()
        if not ssid:
            self._wifi_say("pick a network or type an SSID", err=True); return
        if " " in ssid:
            self._wifi_say("SSID has a space — not supported by the shell cmd",
                           err=True); return
        cmd = "connect3" if self.wifi_wpa3.get_active() else "connect"
        self.send_line("wifi %s %s %s" % (cmd, ssid, psk))
        self._wifi_say("connecting to %s…" % ssid)
        self._wifi_poll = 0
        GLib.timeout_add(2000, self._wifi_poll_status)

    def _wifi_poll_status(self):
        if self.ser is None:
            return False
        self._wifi_status = {}
        self._wifi_linebuf = ""
        self._wifi_capture = "status"
        self.send_line("wifi status")
        GLib.timeout_add(600, self._wifi_apply_status)
        self._wifi_poll += 1
        return self._wifi_poll < 8                      # ~16 s of polling

    def _wifi_apply_status(self):
        self._wifi_capture = None
        link = self._wifi_status.get("link", "")
        if link.startswith("joined"):
            self._wifi_say("✓ %s" % link, ok=True)
            self._wifi_poll = 99                        # stop the poll loop
        elif link.startswith("failed"):
            self._wifi_say("✗ join failed (%s)" % link, err=True)
            self._wifi_poll = 99
        elif link:
            self._wifi_say("%s…" % link)
        return False

    def on_wifi_disconnect(self, _b=None):
        if self.ser is None:
            return
        self.send_line("wifi disconnect")
        self._wifi_poll = 99
        self._wifi_say("disconnect requested")

    # ---- helpers ----------------------------------------------------------
    def _wifi_say(self, text, ok=False, err=False):
        colour = GREEN if ok else ("#ff6b6b" if err else "#888888")
        self.wifi_status_lbl.set_markup(
            "<span foreground='%s'>%s</span>"
            % (colour, GLib.markup_escape_text(text)))

    @staticmethod
    def _wifi_bars(rssi):
        n = 4 if rssi >= -55 else 3 if rssi >= -67 else 2 if rssi >= -78 else 1
        return "█" * n + "░" * (4 - n)
