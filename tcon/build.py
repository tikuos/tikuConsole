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

from tcon.build_options import (BuildFeatures, board_key_for_platform,
                                feature_support, invoker_prefix, make_flags,
                                profile_name)


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
        self.bld_wifi = Gtk.CheckButton(label="WiFi")
        self.bld_basic = Gtk.CheckButton(label="BASIC")
        self.bld_color = Gtk.CheckButton(label="colour")
        self.bld_usb = Gtk.CheckButton(label="USB console")
        self.bld_web = Gtk.CheckButton(label="web (HTTPS)")
        self.bld_ble = Gtk.CheckButton(label="Bluetooth")
        self.bld_pkhw = Gtk.CheckButton(label="HW PK")
        self.bld_flpr = Gtk.CheckButton(label="FLPR")
        self.bld_shell.set_active(True)
        self.bld_net.set_active(True)
        self.bld_color.set_active(True)
        self.bld_wifi.set_active(True)        # RP2350W default: WiFi-capable
        self.bld_usb.set_active(True)         # RP2350 default: native USB CDC
        for w, tip in (
                (self.bld_shell, "interactive shell (TIKU_SHELL_ENABLE=1)"),
                (self.bld_net,   "SLIP/IP + telnet/CoAP/MQTT "
                                 "(TIKU_SHELL_NET_TEST=1). RP2350: superseded by "
                                 "WiFi — its autostarted servers starve the "
                                 "cooperative USB console."),
                (self.bld_wifi,  "RP2350W only: CYW43 WiFi + lean IP stack "
                                 "(DHCP/DNS/NTP) for the WiFi panel. Drops the "
                                 "heavy net servers so the USB console stays live."),
                (self.bld_basic, "Tiku BASIC interpreter "
                                 "(TIKU_SHELL_BASIC_ENABLE=1)"),
                (self.bld_color, "ANSI colour in the shell "
                                 "(TIKU_SHELL_COLOR=1)"),
                (self.bld_usb,   "RP2350 only: put the console on the native USB "
                                 "CDC (TIKU_CONSOLE=usb) — the port this app "
                                 "connects to. Uncheck only for an external "
                                 "UART/FT232 rig."),
                (self.bld_web,   "HTTPS web stack: cert-TLS 1.3/1.2 + HTTP + DNS "
                                 "+ time + crypto. Enables BASIC HTTPGET$ and "
                                 "BROWSE (the text web browser); forces BASIC on. "
                                 "Apollo / RP2350 / nRF54L15 only — heavy build."),
                (self.bld_ble,   "RP2350: CYW43439 Bluetooth HCI stack. "
                                 "Apollo510 Blue: EM9305 BLE + Nordic UART "
                                 "Service wireless shell."),
                (self.bld_pkhw,  "nRF54L15 only: compile the CRACEN BA414EP "
                                 "hardware ECDSA-verify path (481x over software, "
                                 "TIKU_CRACEN_PK_ENABLE=1). The engine is "
                                 "microcoded and TikuOS ships NO microcode "
                                 "(Nordic-proprietary): you must ALSO drop your "
                                 "licensed arch/nordic/cracen_pk_microcode.h, or "
                                 "it fails safe to software. Standalone/opt-in; "
                                 "not auto-wired into the TLS path."),
                (self.bld_flpr,  "nRF54L15 only: build the FLPR RISC-V "
                                 "coprocessor image (TIKU_FLPR_ENABLE=1); "
                                 "requires the RISC-V cross-toolchain.")):
            w.set_tooltip_text(tip)
            frow.append(w)

        # web (HTTPS) forces BASIC on (HTTPGET$/BROWSE are BASIC builtins), so
        # make that visible instead of silently overriding an unchecked box:
        # while web is checked, tick BASIC and lock it (insensitive); restore
        # user control when web is unchecked.
        def _web_locks_basic(*_):
            on = self.bld_web.get_active()
            if on:
                self.bld_basic.set_active(True)
            self.bld_basic.set_sensitive(not on)
        self.bld_web.connect("toggled", _web_locks_basic)
        for w in (self.bld_net, self.bld_wifi, self.bld_web,
                  self.bld_ble, self.bld_flpr):
            w.connect("toggled", self._bld_update_dependencies)
        _web_locks_basic()

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
            for rb in self.bld_board_radios:
                rb.connect("toggled", self._bld_board_toggled)
            self._select_default_board()
            self._bld_gate_features()
        return box

    def _select_default_board(self):
        """Preselect the MCU matching a currently-attached device, if any."""
        idx = 0
        try:
            from tcon.ports import scan_ports, identify_port
            for p in scan_ports():
                plat = identify_port(p)[0].lower()
                key = board_key_for_platform(plat)
                if key and key in self.bld_boards:
                    idx = self.bld_boards.index(key)
                    break
        except Exception:
            pass
        if self.bld_board_radios:
            self.bld_board_radios[idx].set_active(True)

    def _bld_selected_board(self):
        """The board object for the currently-selected MCU radio (or None)."""
        if _TB is None:
            return None
        idx = next((i for i, rb in enumerate(self.bld_board_radios)
                    if rb.get_active()), -1)
        if 0 <= idx < len(self.bld_boards):
            return _TB.resolve_board(self.bld_boards[idx])
        return None

    def _bld_gate_features(self, *_):
        """Grey out feature toggles the selected board cannot do.

        WiFi (CYW43) and the native USB CDC console exist only on the
        RP2350W; on MSP430 / Apollo they are meaningless, so disable and clear
        them (the flags already guard on family, but the UI should say so). On
        RP2350 both default on -- the USB console is the port this app
        connects to."""
        board = self._bld_selected_board()
        if board is None or not hasattr(self, "bld_wifi"):
            return
        support = feature_support(board)
        is_rp = board.family == "rp2350"
        for w in (self.bld_wifi, self.bld_usb):
            w.set_sensitive(is_rp)
            w.set_active(is_rp)
        for name, widget in (("web", self.bld_web),
                             ("bluetooth", self.bld_ble),
                             ("pkhw", self.bld_pkhw),
                             ("flpr", self.bld_flpr)):
            widget.set_sensitive(support[name])
            if not support[name]:
                widget.set_active(False)
        self._bld_update_dependencies()

    def _bld_board_toggled(self, rb):
        if rb.get_active():
            self._bld_gate_features()

    def _bld_update_dependencies(self, *_):
        """Expose implicit shell/network dependencies in the controls."""
        if not hasattr(self, "bld_shell"):
            return
        needs_shell = any(w.get_active() for w in
                          (self.bld_net, self.bld_wifi, self.bld_web,
                           self.bld_ble, self.bld_flpr))
        if needs_shell:
            self.bld_shell.set_active(True)
        self.bld_shell.set_sensitive(not needs_shell)
        self.bld_net.set_sensitive(not (self.bld_wifi.get_active()
                                        or self.bld_web.get_active()))

    def _bld_features(self):
        return BuildFeatures(
            shell=self.bld_shell.get_active(),
            networking=self.bld_net.get_active(),
            wifi=self.bld_wifi.get_active(),
            basic=self.bld_basic.get_active(),
            color=self.bld_color.get_active(),
            usb=self.bld_usb.get_active(),
            web=self.bld_web.get_active(),
            bluetooth=self.bld_ble.get_active(),
            pkhw=self.bld_pkhw.get_active(),
            flpr=self.bld_flpr.get_active(),
        )

    def _bld_flags(self, board):
        """Translate the MCU + feature checkboxes into make variables.

        Features whose Makefile default can be ON -- shell (always) and BASIC
        (defaults ON on Apollo/ambiq) -- are emitted explicitly as =0/1 so an
        unchecked box actively turns the feature off rather than letting the
        platform default win.  colour is explicit too for the same reason."""
        # Flash the board at the baud selected in the toolbar.  Fall back to
        # the board default if the field is not numeric.
        try:
            _ubaud = int(str(self.baud.get_text()).strip())
        except (ValueError, AttributeError, TypeError):
            _ubaud = board.default_baud
        self._bld_baud = _ubaud                 # for the post-flash reconnect
        return make_flags(board, self._bld_features(), _ubaud)

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

        profile = profile_name(board, self._bld_features())

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
            # The application may be root only for TUN/NAT.  Match TikuBench:
            # build/flash as the invoking user so toolchains/plugins resolve
            # from their HOME and no root-owned build artifacts are left.
            proc = launcher.spawnv(invoker_prefix() + argv)
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
        # Reconnect at the baud the firmware was *built* with, not the board
        # default -- otherwise a non-9600 UART_BAUD build (e.g. 115200) comes
        # up as garbage and you have to fix the baud + reconnect by hand.  The
        # sticky flag stops the post-flash port rescan from reverting it.
        self.baud.set_text(str(getattr(self, "_bld_baud",
                                       self._bld_board.default_baud)))
        self._bld_baud_sticky = True
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
