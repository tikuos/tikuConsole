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
        self._wifi_capture = None      # None|"list"|"status"|"ip"|"ping"|"ntp"
        self._wifi_aps = []
        self._wifi_status = {}
        self._wifi_poll = 0
        # "On the network" state: DHCP lease + quick internet checks over WiFi.
        self._wifi_ip = None           # last IPv4 read from the board's `ip`
        self._wifi_ip_tries = 0
        self._wifi_pingsum = None      # last board-side ping summary line
        self._wifi_ntp = None          # last board-side ntp result line
        self._wifi_auto_up = True      # a successful join also brings IP up
        # Drives the main-row WiFi light + IP chip (see _wifi_update_led).
        self._wifi_joined = False      # last known link state
        self._wifi_ip_shown = None     # stable IP for the chip (not the parse buf)

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
        elif self._wifi_capture == "ip":
            # the board's `ip` prints "IPv4: 192.168.1.115"
            if "IPv4:" in line:
                self._wifi_ip = line.split("IPv4:", 1)[1].strip()
        elif self._wifi_capture == "ping":
            # summary line: "--- 8.8.8.8 ping: 4 sent, 4 received ---"
            if "sent," in line and "received" in line:
                self._wifi_pingsum = line.strip().strip("-").strip()
        elif self._wifi_capture == "ntp":
            # result: "ntp: 2026-06-26 19:57:34 UTC  stratum 1" (or an error)
            s = line.strip()
            if s.startswith("ntp:"):
                self._wifi_ntp = s[4:].strip()

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
            self._wifi_joined = True
            self._wifi_update_led()
            if self._wifi_auto_up:                      # join → also go online
                GLib.timeout_add(700, self.on_wifi_online)
        elif link.startswith("failed"):
            self._wifi_say("✗ join failed (%s)" % link, err=True)
            self._wifi_poll = 99
            self._wifi_joined = False
            self._wifi_ip_shown = None
            self._wifi_update_led()
        elif link:
            self._wifi_say("%s…" % link)
        return False

    def on_wifi_disconnect(self, _b=None):
        if self.ser is None:
            return
        self.send_line("wifi disconnect")
        self._wifi_poll = 99
        self._wifi_ip = None
        self._wifi_joined = False
        self._wifi_ip_shown = None
        self._wifi_update_led()
        self.wifi_ip_lbl.set_markup(
            "<span foreground='#888888'>not on the network</span>")
        self._wifi_say("disconnect requested")

    def _wifi_update_led(self):
        """Drive the main-row WiFi light + IP chip from the link/lease state:
        green = joined and online (has a lease), amber = joined but no IP,
        red = down.  The IP address rides next to the lights, not in the panel."""
        if not hasattr(self, "wifi_led"):
            return
        joined = self._wifi_joined and self.ser is not None
        ip = self._wifi_ip_shown if joined else None
        if joined and ip:
            colour = GREEN                              # online
        elif joined:
            colour = "#e6b800"                          # joined, no lease (amber)
        else:
            colour = "#ff6b6b"                          # down
        self.wifi_led.set_markup("<span foreground='%s'>●</span> WiFi" % colour)
        self.wifi_ip_chip.set_markup(
            ("<span foreground='%s' size='small'>%s</span>" % (GREEN, ip))
            if ip else "")

    # ---- sync the lights to the board on connect --------------------------
    def _wifi_sync(self):
        """One-shot after connect: read the board's WiFi link (and IP if up) so
        the main-row light + IP chip reflect reality -- the board may have
        cold-boot auto-rejoined and already hold a lease.  Quiet: never writes
        the panel's status labels, so it can't stomp a message the user sees."""
        if self.ser is None or self._wifi_capture is not None:
            return False                                # don't fight a live query
        self._wifi_status = {}
        self._wifi_linebuf = ""
        self._wifi_capture = "status"
        self.send_line("wifi status")
        GLib.timeout_add(700, self._wifi_sync_apply)
        return False

    def _wifi_sync_apply(self):
        self._wifi_capture = None
        link = self._wifi_status.get("link", "")
        self._wifi_joined = link.startswith("joined")
        self._wifi_update_led()
        if self._wifi_joined:                           # peek at the live IP
            self._wifi_ip = None
            self._wifi_linebuf = ""
            self._wifi_capture = "ip"
            self.send_line("ip")
            GLib.timeout_add(700, self._wifi_sync_ip)
        return False

    def _wifi_sync_ip(self):
        self._wifi_capture = None
        if self._wifi_ip and self._wifi_ip != "0.0.0.0":
            self._wifi_ip_shown = self._wifi_ip
            self._wifi_update_led()
        return False

    # ---- On the network: DHCP lease + ping + NTP over WiFi ----------------
    def on_wifi_online(self, _b=None):
        """Bring the board's IP stack up over the joined radio (`wifi up`) and
        read back the DHCP lease.  Triggered by the Go-online button and
        automatically after a successful join."""
        if self.ser is None:
            self._wifi_net_say("connect to a board first", err=True); return
        self.wifi_up_btn.set_sensitive(False)
        self._wifi_ip_tries = 0
        self._wifi_net_say("bringing IP up over WiFi…")
        self.send_line("wifi up")
        GLib.timeout_add(1500, self._wifi_read_ip)      # let DHCP bind (~1.2s)

    def _wifi_read_ip(self):
        if self.ser is None:
            self.wifi_up_btn.set_sensitive(True); return False
        self._wifi_ip = None
        self._wifi_linebuf = ""
        self._wifi_capture = "ip"
        self.send_line("ip")
        GLib.timeout_add(700, self._wifi_apply_ip)
        return False

    def _wifi_apply_ip(self):
        self._wifi_capture = None
        self._wifi_ip_tries += 1
        if self._wifi_ip and self._wifi_ip != "0.0.0.0":
            self._wifi_ip_shown = self._wifi_ip         # IP rides by the lights
            self._wifi_update_led()
            self.wifi_ip_lbl.set_markup(
                "<span foreground='%s'>● online</span>" % GREEN)
            self._wifi_net_say("DHCP lease: %s" % self._wifi_ip, ok=True)
            self.wifi_up_btn.set_sensitive(True)
        elif self._wifi_ip_tries < 4:
            GLib.timeout_add(1200, self._wifi_read_ip)  # poll until it binds
        else:
            self._wifi_ip_shown = None
            self._wifi_update_led()
            self.wifi_ip_lbl.set_markup(
                "<span foreground='#ff6b6b'>no lease — is the AP's DHCP up?"
                "</span>")
            self._wifi_net_say("no DHCP lease (retry Go online)", err=True)
            self.wifi_up_btn.set_sensitive(True)
        return False

    def on_wifi_ping_net(self, _b=None):
        """Have the board ping a host over its OWN WiFi (not the host TUN)."""
        if self.ser is None:
            self._wifi_net_say("connect to a board first", err=True); return
        target = self.wifi_ping_t.get_text().strip() or "8.8.8.8"
        self.wifi_ping_btn.set_sensitive(False)
        self._wifi_pingsum = None
        self._wifi_linebuf = ""
        self._wifi_capture = "ping"
        self.send_line("ping %s" % target)
        self._wifi_net_say("pinging %s over WiFi…" % target)
        GLib.timeout_add(7000, self._wifi_apply_ping)   # 4 pings ~5s + margin

    def _wifi_apply_ping(self):
        self._wifi_capture = None
        self.wifi_ping_btn.set_sensitive(True)
        s = self._wifi_pingsum
        if s:
            m = re.search(r"(\d+)\s+sent,\s+(\d+)\s+received", s)
            ok = bool(m) and int(m.group(2)) > 0
            self._wifi_net_say(("✓ " if ok else "✗ ") + s, ok=ok, err=not ok)
        else:
            self._wifi_net_say("ping: no reply (timed out)", err=True)
        return False

    def on_wifi_ntp(self, _b=None):
        """Fetch UTC time from an internet NTP server, over WiFi."""
        if self.ser is None:
            self._wifi_net_say("connect to a board first", err=True); return
        self.wifi_ntp_btn.set_sensitive(False)
        self._wifi_ntp = None
        self._wifi_linebuf = ""
        self._wifi_capture = "ntp"
        self.send_line("ntp")
        self._wifi_net_say("fetching time over WiFi…")
        GLib.timeout_add(9000, self._wifi_apply_ntp)    # dns+ntp up to ~8s

    def _wifi_apply_ntp(self):
        self._wifi_capture = None
        self.wifi_ntp_btn.set_sensitive(True)
        s = self._wifi_ntp
        if s and ("UTC" in s or "stratum" in s):
            self._wifi_net_say("🕒 %s" % s, ok=True)
        elif s:
            self._wifi_net_say("ntp: %s" % s, err=True)
        else:
            self._wifi_net_say("ntp: no reply (timed out)", err=True)
        return False

    # ---- helpers ----------------------------------------------------------
    def _wifi_say(self, text, ok=False, err=False):
        colour = GREEN if ok else ("#ff6b6b" if err else "#888888")
        self.wifi_status_lbl.set_markup(
            "<span foreground='%s'>%s</span>"
            % (colour, GLib.markup_escape_text(text)))

    def _wifi_net_say(self, text, ok=False, err=False):
        """Status line for the 'On the network' (lease/ping/ntp) actions."""
        colour = GREEN if ok else ("#ff6b6b" if err else "#888888")
        self.wifi_net_lbl.set_markup(
            "<span foreground='%s'>%s</span>"
            % (colour, GLib.markup_escape_text(text)))

    @staticmethod
    def _wifi_bars(rssi):
        n = 4 if rssi >= -55 else 3 if rssi >= -67 else 2 if rssi >= -78 else 1
        return "█" * n + "░" * (4 - n)
