"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.files - FilesMixin: a /data file-browser window (a small file manager).

Opened from the "Files..." button in the connection bar.  Lists the device's
/data store and downloads / uploads / deletes files over the shell console
using the shared protocol in tikufs.py (the same code the tikufs CLI drives).
The board is half-duplex request/response, so each operation pauses the async
console read-watch -- handing the serial port to tikufs for the exchange --
then restores it (the watch ran the port non-blocking; tikufs wants a real read
timeout, so we flip ser.timeout around the call).

SPDX-License-Identifier: Apache-2.0
"""
import os
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib, Pango  # noqa: E402

import tikufs  # noqa: E402  -- shared device-fs protocol (CLI + GUI)


class FilesMixin:
    # ---- console read-watch guard ----------------------------------------
    def _files_pause(self):
        """Pause the async console reader and hand back the serial port (or
        None if not connected).  tikufs then owns the port for the exchange."""
        if self.ser is None:
            return None
        if self.ser_src:
            GLib.source_remove(self.ser_src); self.ser_src = 0
        self.ser.timeout = 0.2          # watch ran non-blocking; tikufs blocks
        return self.ser

    def _files_resume(self):
        if self.ser is not None:
            self.ser.timeout = 0
            if not self.ser_src:
                self.ser_src = GLib.unix_fd_add_full(
                    GLib.PRIORITY_DEFAULT, self.ser.fileno(),
                    GLib.IOCondition.IN, self._on_serial)

    # ---- small helpers ----------------------------------------------------
    @staticmethod
    def _icon_label_button(icon, label):
        btn = Gtk.Button()
        box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        box.set_halign(Gtk.Align.CENTER)
        box.append(Gtk.Image.new_from_icon_name(icon))
        box.append(Gtk.Label(label=label))
        btn.set_child(box)
        return btn

    def _files_set_status(self, msg):
        if getattr(self, "files_status", None):
            self.files_status.set_text(msg)

    def _files_set_actions(self, on):
        if getattr(self, "files_dl_btn", None):
            self.files_dl_btn.set_sensitive(on)
        if getattr(self, "files_del_btn", None):
            self.files_del_btn.set_sensitive(on)

    def _files_selected(self):
        """Selected filename (read from the live row's label), or None."""
        row = self.files_list.get_selected_row()
        if not row:
            return None
        box = row.get_child()                       # HBox [icon, label]
        lbl = box.get_last_child() if box else None
        return lbl.get_text() if lbl else None

    def _files_clear(self):
        row = self.files_list.get_row_at_index(0)
        while row is not None:
            self.files_list.remove(row)
            row = self.files_list.get_row_at_index(0)

    def _files_set_usage(self, df_txt):
        """Distil `df` into a one-line header subtitle."""
        summary = "the device file store"
        for ln in df_txt.split("\n"):
            if ln.lstrip().startswith("/data"):
                tok = ln.split()                    # /data Size Used Avail % Files Backing
                if len(tok) >= 6:
                    summary = "%s files · %s used · %s free" % (
                        tok[5], tok[2], tok[3])
                break
        if getattr(self, "files_usage", None):
            self.files_usage.set_text(summary)

    # ---- refresh ----------------------------------------------------------
    def _files_refresh(self, *a):
        ser = self._files_pause()
        if ser is None:
            if getattr(self, "files_usage", None):
                self.files_usage.set_text("not connected")
            self._files_set_status("Not connected -- press Connect first")
            self._files_clear()
            self._files_set_actions(False)
            return
        try:
            df_txt = tikufs.df(ser)
            ls_txt = tikufs.ls(ser)
        finally:
            self._files_resume()

        self._files_set_usage(df_txt)
        self._files_clear()
        self._files_set_actions(False)              # rebuild clears the selection
        for name in tikufs.parse_names(ls_txt):
            row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
            for m in ("top", "bottom", "start", "end"):
                getattr(row, "set_margin_" + m)(4)
            row.append(Gtk.Image.new_from_icon_name("text-x-generic-symbolic"))
            lbl = Gtk.Label(label=name); lbl.set_xalign(0); lbl.set_hexpand(True)
            row.append(lbl)
            self.files_list.append(row)

    # ---- selection --------------------------------------------------------
    def _files_on_select(self, _lb, row):
        self._files_set_actions(row is not None)

    def _files_on_activate(self, _lb, _row):
        self._files_download()                      # double-click / Enter

    # ---- delete -----------------------------------------------------------
    def _files_delete(self, *a):
        name = self._files_selected()
        if not name:
            self._files_set_status("select a file to delete"); return
        ser = self._files_pause()
        if ser is None:
            self._files_set_status("not connected"); return
        try:
            tikufs.rm(ser, name)
            msg = "deleted %s" % name
        except tikufs.FsError as e:
            msg = "delete failed: %s" % e
        finally:
            self._files_resume()
        self._files_refresh()
        self._files_set_status(msg)

    # ---- download (device send -> host, via a Save dialog) ----------------
    def _files_download(self, *a):
        name = self._files_selected()
        if not name:
            self._files_set_status("select a file to download"); return
        self._files_xfer_name = name
        dlg = Gtk.FileDialog()
        dlg.set_initial_name(name)
        dlg.save(self.files_win, None, self._files_download_done)

    def _files_download_done(self, dlg, res, *a):
        try:
            gfile = dlg.save_finish(res)
        except GLib.Error:
            return                                  # cancelled / dismissed
        if gfile is None:
            return
        path = gfile.get_path()
        ser = self._files_pause()
        if ser is None:
            self._files_set_status("not connected"); return
        try:
            data = tikufs.get(ser, self._files_xfer_name)
            with open(path, "wb") as fh:
                fh.write(data)
            msg = "downloaded %d B -> %s" % (len(data), path)
        except tikufs.FsError as e:
            msg = "download failed: %s" % e
        except OSError as e:
            msg = "cannot write %s: %s" % (path, e)
        finally:
            self._files_resume()
        self._files_set_status(msg)

    # ---- upload (host -> device recv, via an Open dialog) -----------------
    def _files_upload(self, *a):
        dlg = Gtk.FileDialog()
        dlg.open(self.files_win, None, self._files_upload_done)

    def _files_upload_done(self, dlg, res, *a):
        try:
            gfile = dlg.open_finish(res)
        except GLib.Error:
            return
        if gfile is None:
            return
        path = gfile.get_path()
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError as e:
            self._files_set_status("cannot read %s: %s" % (path, e)); return
        name = os.path.basename(path)
        if len(data) > tikufs.SLOT_MAX:
            self._files_set_status("%s is %d B > %d B (one slot)" %
                                   (name, len(data), tikufs.SLOT_MAX)); return
        ser = self._files_pause()
        if ser is None:
            self._files_set_status("not connected"); return
        try:
            tikufs.put(ser, name, data)
            msg = "uploaded %d B -> /data/%s" % (len(data), name)
        except tikufs.FsError as e:
            msg = "upload failed: %s" % e
        finally:
            self._files_resume()
        self._files_refresh()
        self._files_set_status(msg)

    # ---- window lifecycle -------------------------------------------------
    def _files_on_destroy(self, *a):
        self.files_win = None
        self.files_list = None
        self.files_usage = None
        self.files_status = None
        self.files_dl_btn = None
        self.files_del_btn = None

    def open_files_window(self, *a):
        if getattr(self, "files_win", None):        # already open -> raise it
            self.files_win.present()
            self._files_refresh()
            return

        win = Gtk.Window(title="Files")
        win.set_default_size(440, 500)
        win.set_transient_for(self.win)
        win.connect("destroy", self._files_on_destroy)
        self.files_win = win

        # header bar: path title + usage subtitle, Refresh / Upload
        hb = Gtk.HeaderBar()
        titlebox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        titlebox.set_valign(Gtk.Align.CENTER)
        t1 = Gtk.Label(label="/data"); t1.add_css_class("title")
        self.files_usage = Gtk.Label(label="the device file store")
        self.files_usage.add_css_class("subtitle")
        titlebox.append(t1); titlebox.append(self.files_usage)
        hb.set_title_widget(titlebox)
        b_ref = Gtk.Button.new_from_icon_name("view-refresh-symbolic")
        b_ref.set_tooltip_text("Refresh the listing")
        b_ref.connect("clicked", self._files_refresh)
        hb.pack_start(b_ref)
        b_up = Gtk.Button.new_from_icon_name("list-add-symbolic")
        b_up.set_tooltip_text("Upload a file to the device")
        b_up.connect("clicked", self._files_upload)
        hb.pack_end(b_up)
        win.set_titlebar(hb)

        # body: scrolled file list
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        win.set_child(box)
        sw = Gtk.ScrolledWindow(); sw.set_vexpand(True)
        sw.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.files_list = Gtk.ListBox()
        self.files_list.set_selection_mode(Gtk.SelectionMode.SINGLE)
        self.files_list.add_css_class("navigation-sidebar")
        ph = Gtk.Label(label="No files in /data"); ph.add_css_class("dim-label")
        ph.set_margin_top(28); ph.set_margin_bottom(28)
        self.files_list.set_placeholder(ph)
        self.files_list.connect("row-selected", self._files_on_select)
        self.files_list.connect("row-activated", self._files_on_activate)
        sw.set_child(self.files_list)
        box.append(sw)

        # action bar: status + selection-gated Download / Delete
        ab = Gtk.ActionBar()
        self.files_status = Gtk.Label(label="")
        self.files_status.add_css_class("dim-label")
        self.files_status.set_ellipsize(Pango.EllipsizeMode.END)
        ab.pack_start(self.files_status)
        self.files_del_btn = self._icon_label_button("user-trash-symbolic",
                                                     "Delete")
        self.files_del_btn.add_css_class("destructive-action")
        self.files_del_btn.set_sensitive(False)
        self.files_del_btn.connect("clicked", self._files_delete)
        ab.pack_end(self.files_del_btn)
        self.files_dl_btn = self._icon_label_button("document-save-symbolic",
                                                    "Download")
        self.files_dl_btn.set_sensitive(False)
        self.files_dl_btn.connect("clicked", self._files_download)
        ab.pack_end(self.files_dl_btn)
        box.append(ab)

        win.present()
        self._files_refresh()
