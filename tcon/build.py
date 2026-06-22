"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.build - BuildMixin: compile + flash TikuOS firmware from the console.

A "Firmware" bar above the connection bar: pick a board and a firmware
profile, flip a couple of options (colour, BASIC), and hit Build & Flash.
The make/flash output streams live into the same console view, and on a
clean flash the console auto-connects to the freshly flashed board -- pick
board, hit go, you are in the shell.

The board table, build directory, and proven make-flag sets are reused
from TikuBench (tikubench.core.board): one source of truth for MCU names
and the eZ-FET / BOOTSEL / J-Link flash paths.  Only the *execution* is
re-driven here, through Gio.Subprocess, so the GTK loop stays live and the
output streams -- TikuBench's own build_and_flash() is a blocking CLI call
that would freeze the UI.

SPDX-License-Identifier: Apache-2.0
"""
import os
import sys
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib, Gio  # noqa: E402


# --- locate + import TikuBench's board engine (sibling repo under the root) ---
def _import_tikubench():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))          # tikuOS root
    tb = os.environ.get("TIKUBENCH_DIR") or os.path.join(root, "TikuBench")
    if os.path.isdir(os.path.join(tb, "tikubench")) and tb not in sys.path:
        sys.path.insert(0, tb)
    try:
        from tikubench.core import board as tb_board
        return tb_board, None
    except Exception as e:                                  # TikuBench absent
        return None, e


_TB, _TB_ERR = _import_tikubench()


# Build is composed from the feature checkboxes (see on_build_flash), which
# map to the documented Makefile knobs: "shell" is TIKU_SHELL_ENABLE; "networking"
# is the proven TIKU_SHELL_NET_TEST combo (interactive shell PLUS
# SLIP/telnet/CoAP/MQTT, family-agnostic); "BASIC" and "colour" are opt-in.


class BuildMixin:
    # ---- UI: the "Firmware" build/flash bar -------------------------------
    def _build_buildbar(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)

        # Row 1: MCU as a radio group -- one per TikuBench board.
        mrow = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        mrow.append(Gtk.Label(label="MCU"))
        self.bld_boards = list(_TB.BOARDS) if _TB else []
        self.bld_board_radios = []
        group = None
        for key in self.bld_boards:
            rb = Gtk.CheckButton(label=key)        # CheckButton + group = radio
            if group is None:
                group = rb
            else:
                rb.set_group(group)
            self.bld_board_radios.append(rb)
            mrow.append(rb)
        if not self.bld_boards:
            mrow.append(Gtk.Label(label="(TikuBench not found)"))
        box.append(mrow)

        # Row 2: feature checkboxes + the Build & Flash button.
        frow = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        frow.append(Gtk.Label(label="Build"))
        self.bld_shell = Gtk.CheckButton(label="shell")
        self.bld_net = Gtk.CheckButton(label="networking")
        self.bld_basic = Gtk.CheckButton(label="BASIC")
        self.bld_color = Gtk.CheckButton(label="colour")
        self.bld_shell.set_active(True)
        self.bld_net.set_active(True)
        self.bld_color.set_active(True)
        for w, tip in (
                (self.bld_shell, "interactive shell (TIKU_SHELL_ENABLE=1)"),
                (self.bld_net,   "SLIP/IP + telnet/CoAP/MQTT "
                                 "(TIKU_SHELL_NET_TEST=1)"),
                (self.bld_basic, "Tiku BASIC interpreter "
                                 "(TIKU_SHELL_BASIC_ENABLE=1)"),
                (self.bld_color, "ANSI colour in the shell "
                                 "(TIKU_SHELL_COLOR=1)")):
            w.set_tooltip_text(tip)
            frow.append(w)

        self.bld_btn = Gtk.Button(label="Build & Flash")
        self.bld_btn.add_css_class("suggested-action")
        self.bld_btn.set_hexpand(True)
        self.bld_btn.set_halign(Gtk.Align.END)
        self.bld_btn.connect("clicked", self.on_build_flash)
        frow.append(self.bld_btn)
        box.append(frow)

        self._bld_running = False
        if _TB is None:
            self.bld_btn.set_sensitive(False)
            self.bld_btn.set_tooltip_text(
                "TikuBench not found next to tikuConsole (%s) -- set "
                "TIKUBENCH_DIR to enable build/flash." % (_TB_ERR,))
        else:
            self._select_default_board()
        return box

    def _select_default_board(self):
        """Preselect the MCU matching a currently-attached device, if any."""
        idx = 0
        try:
            from tcon.ports import scan_ports, identify_port
            for p in scan_ports():
                plat = identify_port(p)[0].lower()
                key = ("apollo4l" if "apollo" in plat else
                       "rp2350" if ("rp2" in plat or "pico" in plat) else
                       "msp430fr5994" if "msp" in plat else None)
                if key and key in self.bld_boards:
                    idx = self.bld_boards.index(key)
                    break
        except Exception:
            pass
        if self.bld_board_radios:
            self.bld_board_radios[idx].set_active(True)

    def _bld_flags(self, board):
        """Translate the MCU + feature checkboxes into make variables.

        Features whose Makefile default can be ON -- shell (always) and BASIC
        (defaults ON on Apollo/ambiq) -- are emitted explicitly as =0/1 so an
        unchecked box actively turns the feature off rather than letting the
        platform default win.  colour is explicit too for the same reason."""
        flags = [
            "HAS_TESTS=0", "HAS_EXAMPLES=0",
            "TIKU_SHELL_ENABLE=%d"       % (1 if self.bld_shell.get_active()
                                            else 0),
            "TIKU_SHELL_BASIC_ENABLE=%d" % (1 if self.bld_basic.get_active()
                                            else 0),
            "TIKU_SHELL_COLOR=%d"        % (1 if self.bld_color.get_active()
                                            else 0),
        ]
        if self.bld_net.get_active():
            flags += ["TIKU_KIT_NET_ENABLE=1", "TIKU_SHELL_NET_TEST=1"]
        if self.bld_basic.get_active() and board.family == "msp430":
            flags.append("MEMORY_MODEL=large")      # BASIC needs it on MSP430
        flags.append("MCU=%s" % board.mcu)
        flags.append("UART_BAUD=%d" % board.default_baud)
        return flags

    # ---- build + flash, streamed into the console -------------------------
    def on_build_flash(self, _btn):
        if self._bld_running or _TB is None:
            return
        idx = next((i for i, rb in enumerate(self.bld_board_radios)
                    if rb.get_active()), -1)
        if not (0 <= idx < len(self.bld_boards)):
            self._set_status("pick an MCU first", err=True)
            return
        board = _TB.resolve_board(self.bld_boards[idx])
        flags = self._bld_flags(board)

        feats = [n for n, w in (("shell", self.bld_shell),
                                ("net", self.bld_net),
                                ("BASIC", self.bld_basic),
                                ("colour", self.bld_color))
                 if w.get_active()] or ["bare"]
        profile = "+".join(feats)

        # Free the port: flashing drives the same USB debugger / back-channel
        # the console holds open (eZ-FET ACM, J-Link VCOM).  Reconnect after.
        if self.ser is not None:
            self._teardown()

        # Mirror TikuBench's build_and_flash() command sequence exactly:
        # clean -> build -> flash, with identical flags on build and flash.
        self._bld_board = board
        self._bld_steps = [
            ("make clean", ["make", "clean"]),
            ("make " + " ".join(flags), ["make"] + flags),
            ("make flash", ["make", "flash"] + flags),
        ]
        self._bld_running = True
        self.bld_btn.set_sensitive(False)
        self.bld_btn.set_label("Building…")
        self.cview.remove_css_class("console-off")          # show output now
        self._set_status("building %s (%s)…" % (board.key, profile))
        self.append("\n\x1b[1;34m── build & flash: %s · %s ──\x1b[0m\n"
                    % (board.key, profile))
        self._bld_run_next()

    def _bld_run_next(self):
        if not self._bld_steps:
            return self._bld_done(True)
        label, argv = self._bld_steps.pop(0)
        self.append("\x1b[1;30m$ %s\x1b[0m\n" % label)
        try:
            launcher = Gio.SubprocessLauncher.new(
                Gio.SubprocessFlags.STDOUT_PIPE
                | Gio.SubprocessFlags.STDERR_MERGE)
            launcher.set_cwd(_TB.PROJ_DIR)                  # the tikuOS root
            proc = launcher.spawnv(argv)
        except Exception as e:
            self.append("\x1b[1;31m%s: %s\x1b[0m\n" % (label, e))
            return self._bld_done(False)
        self._bld_pump(proc, proc.get_stdout_pipe())

    def _bld_pump(self, proc, stream):
        stream.read_bytes_async(4096, GLib.PRIORITY_DEFAULT, None,
                                self._bld_on_chunk, proc)

    def _bld_on_chunk(self, stream, res, proc):
        try:
            data = stream.read_bytes_finish(res)
        except Exception:
            data = None
        if data is not None and data.get_size():
            # Feed raw bytes to the ANSI-aware console sink (handles colour
            # + CR/BS just like live serial output does).
            self.append(data.get_data().decode("latin-1"))
            return self._bld_pump(proc, stream)
        proc.wait_async(None, self._bld_on_exit)            # EOF -> reap + status

    def _bld_on_exit(self, proc, res):
        try:
            proc.wait_finish(res)
            ok = proc.get_successful()
        except Exception:
            ok = False
        if not ok:
            return self._bld_done(False)
        self._bld_run_next()                                # build -> flash -> end

    def _bld_done(self, ok):
        self._bld_running = False
        self._bld_steps = []
        self.bld_btn.set_sensitive(True)
        self.bld_btn.set_label("Build & Flash")
        if not ok:
            self.append("\x1b[1;31m── build/flash FAILED -- see output above"
                        " ──\x1b[0m\n")
            self._set_status("build/flash failed", err=True)
            return
        self.append("\x1b[1;32m── build + flash OK -- connecting… ──\x1b[0m\n")
        # Match the console baud to the flashed board, then auto-connect.  The
        # board reboots after `make flash` and (on some probes) its USB CDC
        # re-enumerates, so the port can be briefly absent -- poll for it
        # instead of a single shot, then hand over to Connect if it never
        # settles.  (TikuBench waits ~2 s here for the same reason.)
        self.baud.set_text(str(self._bld_board.default_baud))
        self._set_status("flashed %s -- waiting for the port…"
                         % self._bld_board.key)
        self._bld_try = 0
        GLib.timeout_add(1000, self._bld_autoconnect)

    def _bld_autoconnect(self):
        if self.ser is not None:                        # already connected
            return GLib.SOURCE_REMOVE
        self.refresh_ports()
        if self.port_path:
            self.on_connect(None)                       # opens serial + focuses
            if self.ser is not None:
                # Belt-and-suspenders: keep keystrokes in the console, not a
                # stray text field (the key router drops keys while a Gtk.Entry
                # like Baud holds focus).
                GLib.idle_add(self._focus_console)
                return GLib.SOURCE_REMOVE
        self._bld_try += 1
        if self._bld_try >= 10:                         # ~10 s, then hand over
            self.append("\x1b[1;33m[tikuconsole] port not ready -- press "
                        "Connect when the board re-appears.\x1b[0m\n")
            self._set_status("flashed -- press Connect when the port appears",
                             err=True)
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE                      # retry in 1 s
