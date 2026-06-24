"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.files - FilesMixin: a /data file-browser window (a small file manager).

Opened from the "Files..." button in the connection bar.  Presents the device's
/data store as a folder tree (path-as-name -- the store is flat and the device
segments the names, reporting folders with a trailing '/').  Navigate folders,
upload / download / delete files, and make folders, all over the shell console
via the shared protocol in tikufs.py.  Each operation pauses the async console
read-watch (flipping ser.timeout, since the watch runs the port non-blocking)
so tikufs owns the serial port for the exchange, then restores it.

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

    # ---- paths + small helpers -------------------------------------------
    def _files_abspath(self):
        return "/data/" + self.files_cwd if self.files_cwd else "/data"

    def _files_child(self, leaf):
        return self.files_cwd + "/" + leaf if self.files_cwd else leaf

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
        """(name, is_dir) of the selected row, or None.  Row order matches
        self.files_entries, so map by index (robust in PyGObject)."""
        row = self.files_list.get_selected_row()
        if row is None:
            return None
        i = row.get_index()
        if 0 <= i < len(self.files_entries):
            return self.files_entries[i]
        return None

    def _files_clear(self):
        row = self.files_list.get_row_at_index(0)
        while row is not None:
            self.files_list.remove(row)
            row = self.files_list.get_row_at_index(0)

    def _files_set_usage(self, df_txt):
        summary = "the device file store"
        for ln in df_txt.split("\n"):
            if ln.lstrip().startswith("/data"):
                tok = ln.split()                 # /data Size Used Avail % Files Backing
                if len(tok) >= 6:
                    summary = "%s files · %s used · %s free" % (
                        tok[5], tok[2], tok[3])
                break
        if getattr(self, "files_usage", None):
            self.files_usage.set_text(summary)

    # ---- refresh ----------------------------------------------------------
    def _files_refresh(self, *a):
        abs_ = self._files_abspath()
        if getattr(self, "files_path_lbl", None):
            self.files_path_lbl.set_text(abs_)
        if getattr(self, "files_up_btn", None):
            self.files_up_btn.set_sensitive(bool(self.files_cwd))

        ser = self._files_pause()
        if ser is None:
            if getattr(self, "files_usage", None):
                self.files_usage.set_text("not connected")
            self._files_set_status("Not connected -- press Connect first")
            self.files_entries = []
            self._files_clear()
            self._files_set_actions(False)
            return
        try:
            df_txt = tikufs.df(ser)
            entries = tikufs.list_entries(ser, abs_)
        finally:
            self._files_resume()

        self._files_set_usage(df_txt)
        self._files_clear()
        self._files_set_actions(False)
        self.files_entries = entries
        for name, is_dir in entries:
            row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
            for m in ("top", "bottom", "start", "end"):
                getattr(row, "set_margin_" + m)(4)
            row.append(Gtk.Image.new_from_icon_name(
                "folder-symbolic" if is_dir else "text-x-generic-symbolic"))
            lbl = Gtk.Label(label=name); lbl.set_xalign(0); lbl.set_hexpand(True)
            row.append(lbl)
            self.files_list.append(row)

    # ---- navigation -------------------------------------------------------
    def _files_enter(self, folder):
        self.files_cwd = (self.files_cwd + "/" + folder
                          if self.files_cwd else folder)
        self._files_refresh()

    def _files_up(self, *a):
        self.files_cwd = self.files_cwd.rsplit("/", 1)[0] \
            if "/" in self.files_cwd else ""
        self._files_refresh()

    def _files_on_select(self, _lb, row):
        sel = self._files_selected() if row is not None else None
        self._files_set_actions(sel is not None and not sel[1])

    def _files_on_activate(self, _lb, _row):
        sel = self._files_selected()
        if sel is None:
            return
        name, is_dir = sel
        if is_dir:
            self._files_enter(name)
        else:
            self._files_download()

    # ---- delete -----------------------------------------------------------
    def _files_delete(self, *a):
        sel = self._files_selected()
        if sel is None or sel[1]:
            self._files_set_status("select a file to delete"); return
        name = sel[0]
        ser = self._files_pause()
        if ser is None:
            self._files_set_status("not connected"); return
        try:
            tikufs.rm(ser, self._files_child(name))
            msg = "deleted %s" % name
        except tikufs.FsError as e:
            msg = "delete failed: %s" % e
        finally:
            self._files_resume()
        self._files_refresh()
        self._files_set_status(msg)

    # ---- download ---------------------------------------------------------
    def _files_download(self, *a):
        sel = self._files_selected()
        if sel is None or sel[1]:
            self._files_set_status("select a file to download"); return
        self._files_xfer_name = self._files_child(sel[0])
        dlg = Gtk.FileDialog()
        dlg.set_initial_name(sel[0])
        dlg.save(self.files_win, None, self._files_download_done)

    def _files_download_done(self, dlg, res, *a):
        try:
            gfile = dlg.save_finish(res)
        except GLib.Error:
            return
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

    # ---- upload (into the current folder) ---------------------------------
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
        child = self._files_child(os.path.basename(path))
        if len(data) > tikufs.SLOT_MAX:
            self._files_set_status("%s is %d B > %d B (one slot)" %
                                   (os.path.basename(path), len(data),
                                    tikufs.SLOT_MAX)); return
        ser = self._files_pause()
        if ser is None:
            self._files_set_status("not connected"); return
        try:
            tikufs.put(ser, child, data)
            msg = "uploaded %d B -> /data/%s" % (len(data), child)
        except tikufs.FsError as e:
            msg = "upload failed: %s" % e
        finally:
            self._files_resume()
        self._files_refresh()
        self._files_set_status(msg)

    # ---- new folder (mkdir) -----------------------------------------------
    def _files_newfolder(self, *a):
        dlg = Gtk.Window(title="New Folder")
        dlg.set_transient_for(self.files_win)
        dlg.set_modal(True)
        dlg.set_default_size(280, -1)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        for m in ("top", "bottom", "start", "end"):
            getattr(box, "set_margin_" + m)(12)
        dlg.set_child(box)
        entry = Gtk.Entry(); entry.set_placeholder_text("folder name")
        box.append(entry)
        create = Gtk.Button(label="Create")
        create.add_css_class("suggested-action")
        box.append(create)

        def do_create(*_):
            name = entry.get_text().strip()
            if name and "/" not in name:
                ser = self._files_pause()
                if ser is not None:
                    try:
                        tikufs.mkdir(ser, self._files_child(name))
                    except tikufs.FsError as e:
                        self._files_set_status("mkdir failed: %s" % e)
                    finally:
                        self._files_resume()
                self._files_refresh()
            dlg.destroy()

        create.connect("clicked", do_create)
        entry.connect("activate", do_create)
        dlg.present()
        entry.grab_focus()

    # ---- window lifecycle -------------------------------------------------
    def _files_on_destroy(self, *a):
        for attr in ("files_win", "files_list", "files_usage", "files_status",
                     "files_dl_btn", "files_del_btn", "files_path_lbl",
                     "files_up_btn"):
            setattr(self, attr, None)

    def open_files_window(self, *a):
        if getattr(self, "files_win", None):
            self.files_win.present()
            self._files_refresh()
            return
        self.files_cwd = ""
        self.files_entries = []

        win = Gtk.Window(title="Files")
        win.set_default_size(460, 520)
        win.set_transient_for(self.win)
        win.connect("destroy", self._files_on_destroy)
        self.files_win = win

        # header bar: path title + usage subtitle; Up/Refresh left, New/Upload right
        hb = Gtk.HeaderBar()
        titlebox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        titlebox.set_valign(Gtk.Align.CENTER)
        self.files_path_lbl = Gtk.Label(label="/data")
        self.files_path_lbl.add_css_class("title")
        self.files_usage = Gtk.Label(label="the device file store")
        self.files_usage.add_css_class("subtitle")
        titlebox.append(self.files_path_lbl); titlebox.append(self.files_usage)
        hb.set_title_widget(titlebox)

        self.files_up_btn = Gtk.Button.new_from_icon_name("go-up-symbolic")
        self.files_up_btn.set_tooltip_text("Parent folder")
        self.files_up_btn.set_sensitive(False)
        self.files_up_btn.connect("clicked", self._files_up)
        hb.pack_start(self.files_up_btn)
        b_ref = Gtk.Button.new_from_icon_name("view-refresh-symbolic")
        b_ref.set_tooltip_text("Refresh")
        b_ref.connect("clicked", self._files_refresh)
        hb.pack_start(b_ref)

        b_up = Gtk.Button.new_from_icon_name("list-add-symbolic")
        b_up.set_tooltip_text("Upload a file here")
        b_up.connect("clicked", self._files_upload)
        hb.pack_end(b_up)
        b_new = Gtk.Button.new_from_icon_name("folder-new-symbolic")
        b_new.set_tooltip_text("New folder")
        b_new.connect("clicked", self._files_newfolder)
        hb.pack_end(b_new)
        win.set_titlebar(hb)

        # body: scrolled file list
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        win.set_child(box)
        sw = Gtk.ScrolledWindow(); sw.set_vexpand(True)
        sw.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.files_list = Gtk.ListBox()
        self.files_list.set_selection_mode(Gtk.SelectionMode.SINGLE)
        self.files_list.add_css_class("navigation-sidebar")
        ph = Gtk.Label(label="Empty folder"); ph.add_css_class("dim-label")
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
