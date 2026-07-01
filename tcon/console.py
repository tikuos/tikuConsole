"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.console - ConsoleMixin: the colour console.

A non-terminal GtkTextView made to behave like one: a window-level capture key
controller forwards keystrokes straight to the board, a small ANSI/SGR decoder
colours the output, BS/CR are interpreted, and an application-priority CSS rule
forces a true fixed-width font so the bold boot logo lines up like picocom.

SPDX-License-Identifier: Apache-2.0
"""
import re
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gdk, Pango, GLib  # noqa: E402

from tcon import GREEN  # noqa: E402


class ConsoleMixin:
    _CSI = re.compile(r"\x1b\[([0-9;]*)([A-Za-z])")

    def _route_key(self, ctrl, keyval, code, state):
        """Window-level gate: forward keys to the board when connected, but let
        real text fields (baud / ping target / count) keep their keys."""
        if self.ser is None:
            return False
        if isinstance(self.win.get_focus(), Gtk.Editable):
            return False
        return self._on_console_key(ctrl, keyval, code, state)

    def _focus_console(self):
        self.cview.grab_focus()
        return False                               # one-shot idle callback

    def _on_console_key(self, _ctrl, keyval, _code, state):
        """Type straight into the console -- forward each keystroke to the board,
        which echoes it back, so the console behaves like a real terminal."""
        if self.ser is None:
            return False
        ctrl = bool(state & Gdk.ModifierType.CONTROL_MASK)
        # macOS Command key -> Meta (some GDK backends Super); accept it for
        # copy/paste so Cmd-C / Cmd-V work natively. Generic Ctrl-<letter>
        # control codes below stay Ctrl-only.
        cmd = bool(state & (Gdk.ModifierType.META_MASK | Gdk.ModifierType.SUPER_MASK))
        if (ctrl or cmd) and keyval in (Gdk.KEY_c, Gdk.KEY_C):  # copy sel, else ^C
            if self.cbuf.get_has_selection():
                return False
            if ctrl:                                    # bare Ctrl-C = ^C
                self.ser.write(b"\x03")
            return True
        if (ctrl or cmd) and keyval in (Gdk.KEY_v, Gdk.KEY_V):  # paste to board
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
        # Force a true fixed-width font.  set_monospace() only rides the theme's
        # ".monospace" class, which a user font setting can override -- then the
        # view renders proportional and the bold ANSI boot logo (and any
        # space-aligned column output) drifts out of line.  An application-
        # priority font-family wins the cascade; these families also keep their
        # BOLD advance equal to regular, so the bold logo lines up like picocom.
        data = ("textview.console, textview.console text {"
                " background-color:#0b0b0b; color:#cccccc;"
                " font-family:\"DejaVu Sans Mono\",\"Liberation Mono\","
                "\"Noto Sans Mono\",monospace; font-size:11pt; }"
                "textview.console { padding:4px; }"
                # disconnected: dim the whole view to show it is not enterable
                "textview.console.console-off { opacity:0.45; }"
                ".splash { background-image:"
                " linear-gradient(165deg,#ffffff 0%,#f4eee1 100%); }"
                ".splash-body { padding:6px 40px 20px 40px; }"
                ".splash-accent { background-image:"
                " linear-gradient(90deg,#1f6fc4 0%,#2e9e54 52%,#f0a91e 100%); }"
                ".splash-title { font-size:28pt; font-weight:bold; color:#14457f; }"
                ".splash-sub { font-size:12pt; color:#5b6b79; }"
                ".splash-author { font-size:13pt; font-weight:bold; color:#1565c0; }"
                ".splash-lic { font-size:9pt; color:#8a8a8a; }"
                ".splash-load { font-size:11pt; font-weight:bold; color:#2e7d32; }"
                ".splash progressbar > trough,"
                " .splash progressbar > trough > progress { min-height:9px; }"
                ".splash progressbar > trough > progress { background-image:"
                " linear-gradient(90deg,#1f6fc4,#2e9e54,#f0a91e); }"
                # SLIP activity LEDs: idle grey; on a frame they snap bright +
                # glow, then the CSS transition fades them back when traffic stops
                ".slip-led { color:#555; font-weight:bold;"
                " transition:color 500ms ease-out, text-shadow 500ms ease-out; }"
                ".slip-led.tx-on { color:#36c5f0; text-shadow:0 0 8px #36c5f0; }"
                ".slip-led.rx-on { color:#5fd35f; text-shadow:0 0 8px #5fd35f; }")
        try:
            css.load_from_string(data)
        except Exception:
            css.load_from_data(data.encode())
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        palette = {30: "#666666", 31: "#ff6b6b", 32: GREEN, 33: "#fce94f",
                   34: "#729fcf", 35: "#ad7fa8", 36: "#34e2e2", 37: "#d3d7cf"}
        self.tags = {}
        for code, color in palette.items():
            self.tags["fg%d" % code] = self.cbuf.create_tag("fg%d" % code,
                                                            foreground=color)
        self.tags["bold"] = self.cbuf.create_tag("bold", weight=Pango.Weight.BOLD)
        self.tags["dim"] = self.cbuf.create_tag("dim", foreground="#7f7f7f")
        # Auto-follow the tail.  Scrolling right after an insert races the lazy
        # line-height validation and stalls; instead react to the scroll
        # adjustment's own signals.  "changed" fires once the view has
        # re-measured after new text -- the moment we can reliably pin to the
        # bottom; "value-changed" tracks whether the user has scrolled away.
        self._follow = True
        if getattr(self, "cadj", None) is not None:
            self.cadj.connect("value-changed", self._on_scroll_value)
            self.cadj.connect("changed", self._on_scroll_changed)
        self.ansi_pending = ""
        self.cur = []                       # active ANSI tag names
        # Block cursor at the buffer end (where the board echoes typed chars):
        # a reverse-video space that append() strips before inserting and re-adds
        # after, so the real text + backspace are never disturbed.  A 530 ms
        # timer blinks it by toggling the tag; only shown on a live connection.
        self.tags["cursor"] = self.cbuf.create_tag("cursor", background="#cccccc")
        self._cur_present = False
        self._blink_state = True
        GLib.timeout_add(530, self._cursor_blink)

    def append(self, text):
        self._cursor_off()                  # act on the real text end (re-added below)
        # Drive the SLIP light off the board's own status lines.  A small
        # rolling tail keeps it working even if the line spans two reads.
        self.slip_scan = (self.slip_scan + text)[-96:]
        if "SLIP off --" in self.slip_scan:
            self._set_slip_led(False); self.slip_scan = ""
        elif "SLIP on." in self.slip_scan:
            self._set_slip_led(True); self.slip_scan = ""
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
        self._cursor_on()                   # block cursor at the new end

    def _on_scroll_value(self, adj):
        """Track whether the view is pinned to the end (vs. scrolled up)."""
        self._follow = (adj.get_value() + adj.get_page_size()
                        >= adj.get_upper() - 24.0)

    def _on_scroll_changed(self, adj):
        """New content was re-measured: if we were at the end, stay there."""
        if self._follow:
            adj.set_value(adj.get_upper() - adj.get_page_size())

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

    # ----- block cursor: a reverse-video placeholder at the buffer tail ----- #
    def _cursor_on(self):
        """Add the block-cursor placeholder at the end (connected only)."""
        if self._cur_present or self.ser is None:
            return
        it = self.cbuf.get_end_iter()
        self.cbuf.insert_with_tags(it, " ", self.tags["cursor"])
        self._cur_present = True
        self._blink_state = True            # solid right after fresh output

    def _cursor_off(self):
        """Strip the placeholder so inserts/backspace act on the real text."""
        if not self._cur_present:
            return
        end = self.cbuf.get_end_iter()
        start = end.copy()
        if start.backward_char():
            self.cbuf.delete(start, end)
        self._cur_present = False

    def _cursor_blink(self, *_):
        """Toggle the cursor tag ~twice a second; keep it hidden while
        disconnected.  Returns True so the GLib timer keeps firing."""
        if self.ser is None:                # not connected -> no cursor
            self._cursor_off()
            return True
        if not self._cur_present:           # connected but idle -> make one
            self._cursor_on()
            return True
        end = self.cbuf.get_end_iter()
        start = end.copy()
        if not start.backward_char():
            return True
        self._blink_state = not self._blink_state
        if self._blink_state:
            self.cbuf.apply_tag(self.tags["cursor"], start, end)
        else:
            self.cbuf.remove_tag(self.tags["cursor"], start, end)
        return True
