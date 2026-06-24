/*
 * gui_files.c - a /data file-browser window for the TikuConsole GUI.
 *
 * Opened from the "Files..." button in the connection bar.  Presents the
 * device's /data store as a small file manager with folder navigation: a
 * header bar (the current path + live usage, Up / Refresh / New Folder /
 * Upload), an icon list of folders and files, and a bottom action bar with
 * selection-gated Download / Delete.  Folders are virtual (path-as-name): the
 * store is flat and the device segments the names, reporting folders with a
 * trailing '/'.  All I/O goes through fs.c (the C twin of tikufs.py); each
 * transfer pauses the console read-watch so it owns the serial fd.
 *
 * Authors: WEISER Research Group, National University of Singapore
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gui.h"
#include "fs.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* paths + small helpers                                                     */
/* ------------------------------------------------------------------------- */

/* Absolute path of the current folder: "/data" or "/data/<cwd>". */
static void files_abspath(App *app, char *out, size_t sz)
{
    if (app->files_cwd[0]) {
        snprintf(out, sz, "/data/%s", app->files_cwd);
    } else {
        snprintf(out, sz, "/data");
    }
}

/* /data-relative child name of <leaf> in the current folder. */
static void files_child(App *app, const char *leaf, char *out, size_t sz)
{
    if (app->files_cwd[0]) {
        snprintf(out, sz, "%s/%s", app->files_cwd, leaf);
    } else {
        snprintf(out, sz, "%s", leaf);
    }
}

static void files_set_status(App *app, const char *msg)
{
    if (app->files_status) {
        gtk_label_set_text(GTK_LABEL(app->files_status), msg);
    }
}

static GtkWidget *icon_label_button(const char *icon, const char *label)
{
    GtkWidget *btn = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
    gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_button_set_child(GTK_BUTTON(btn), box);
    return btn;
}

/* Selected entry name, with a trailing '/' kept for folders (or NULL). */
static const char *files_selected(App *app)
{
    GtkListBoxRow *row =
        gtk_list_box_get_selected_row(GTK_LIST_BOX(app->files_list));
    if (!row) {
        return NULL;
    }
    GtkWidget *child = gtk_list_box_row_get_child(row);
    return child ? g_object_get_data(G_OBJECT(child), "name") : NULL;
}

static int name_is_dir(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return n > 0 && name[n - 1] == '/';
}

static void files_clear_list(App *app)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(app->files_list)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(app->files_list), c);
    }
}

static void files_set_actions(App *app, gboolean on)
{
    if (app->files_dl_btn) {
        gtk_widget_set_sensitive(app->files_dl_btn, on);
    }
    if (app->files_del_btn) {
        gtk_widget_set_sensitive(app->files_del_btn, on);
    }
}

static void files_set_usage(App *app, const char *df)
{
    char        summary[160];
    const char *line = strstr(df, "/data");
    char        used[24], avail[24], files[24];
    if (line && sscanf(line, "%*s %*s %23s %23s %*s %23s",
                       used, avail, files) == 3) {
        snprintf(summary, sizeof(summary),
                 "%s files \xc2\xb7 %s used \xc2\xb7 %s free", files, used, avail);
    } else {
        snprintf(summary, sizeof(summary), "the device file store");
    }
    if (app->files_usage) {
        gtk_label_set_text(GTK_LABEL(app->files_usage), summary);
    }
}

/* ------------------------------------------------------------------------- */
/* refresh (ls of the current folder + df)                                   */
/* ------------------------------------------------------------------------- */

static void files_refresh(App *app)
{
    char abs[300];
    files_abspath(app, abs, sizeof(abs));
    if (app->files_path_lbl) {
        gtk_label_set_text(GTK_LABEL(app->files_path_lbl), abs);
    }
    if (app->files_up_btn) {
        gtk_widget_set_sensitive(app->files_up_btn, app->files_cwd[0] != '\0');
    }

    int fd = files_pause_console(app);
    if (fd < 0) {
        if (app->files_usage) {
            gtk_label_set_text(GTK_LABEL(app->files_usage), "not connected");
        }
        files_set_status(app, "Not connected -- press Connect first");
        files_clear_list(app);
        files_set_actions(app, FALSE);
        return;
    }
    char df[512], ls[8192];
    fs_df(fd, df, sizeof(df));
    fs_ls_path(fd, abs, ls, sizeof(ls));
    files_resume_console(app);

    files_set_usage(app, df);
    files_clear_list(app);
    files_set_actions(app, FALSE);

    const char *names[160];
    int n = fs_parse_names(ls, names, 160);     /* folders keep a trailing '/' */
    for (int i = 0; i < n; i++) {
        int   dir = name_is_dir(names[i]);
        char  leaf[128];
        snprintf(leaf, sizeof(leaf), "%s", names[i]);
        if (dir && leaf[0]) {
            leaf[strlen(leaf) - 1] = '\0';      /* drop the trailing '/' */
        }
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 4);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);
        gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name(
            dir ? "folder-symbolic" : "text-x-generic-symbolic"));
        GtkWidget *lbl = gtk_label_new(leaf);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(row), lbl);
        g_object_set_data_full(G_OBJECT(row), "name",
                               g_strdup(names[i]), g_free);  /* keeps the '/' */
        gtk_list_box_append(GTK_LIST_BOX(app->files_list), row);
    }
}

static void on_refresh(GtkButton *b, gpointer user)
{
    (void)b;
    files_refresh((App *)user);
}

/* ------------------------------------------------------------------------- */
/* navigation                                                                */
/* ------------------------------------------------------------------------- */

static void files_enter(App *app, const char *folder)   /* folder has no '/' */
{
    char tmp[256];
    if (app->files_cwd[0]) {
        snprintf(tmp, sizeof(tmp), "%s/%s", app->files_cwd, folder);
    } else {
        snprintf(tmp, sizeof(tmp), "%s", folder);
    }
    snprintf(app->files_cwd, sizeof(app->files_cwd), "%s", tmp);
    files_refresh(app);
}

static void on_up(GtkButton *b, gpointer user)
{
    (void)b;
    App  *app   = user;
    char *slash = strrchr(app->files_cwd, '/');
    if (slash) {
        *slash = '\0';
    } else {
        app->files_cwd[0] = '\0';
    }
    files_refresh(app);
}

static void on_download(GtkButton *b, gpointer user);

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer user)
{
    (void)lb;
    (void)row;
    App        *app = user;
    const char *sel = files_selected(app);
    if (!sel) {
        return;
    }
    if (name_is_dir(sel)) {                      /* enter the folder */
        char folder[128];
        snprintf(folder, sizeof(folder), "%s", sel);
        folder[strlen(folder) - 1] = '\0';      /* drop the '/' */
        files_enter(app, folder);
    } else {
        on_download(NULL, app);                 /* download the file */
    }
}

static void on_row_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer user)
{
    (void)lb;
    App *app = user;
    /* enable Download/Delete only for a selected FILE */
    const char *sel = row ? files_selected(app) : NULL;
    files_set_actions(app, sel != NULL && !name_is_dir(sel));
}

/* ------------------------------------------------------------------------- */
/* delete                                                                    */
/* ------------------------------------------------------------------------- */

static void on_delete(GtkButton *b, gpointer user)
{
    (void)b;
    App        *app = user;
    const char *sel = files_selected(app);
    if (!sel || name_is_dir(sel)) {
        files_set_status(app, "select a file to delete");
        return;
    }
    char child[300];
    files_child(app, sel, child, sizeof(child));

    int fd = files_pause_console(app);
    if (fd < 0) {
        files_set_status(app, "not connected");
        return;
    }
    char out[256];
    fs_rm(fd, child, out, sizeof(out));
    files_resume_console(app);

    files_refresh(app);
    char st[400];
    if (strstr(out, "cannot")) {
        snprintf(st, sizeof(st), "delete failed: %s", out);
    } else {
        snprintf(st, sizeof(st), "deleted %s", sel);
    }
    files_set_status(app, st);
}

/* ------------------------------------------------------------------------- */
/* download (device send -> host file, via a Save dialog)                    */
/* ------------------------------------------------------------------------- */

static void on_download_done(GObject *src, GAsyncResult *res, gpointer user)
{
    App   *app  = user;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file) {
        return;
    }
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) {
        return;
    }
    int fd = files_pause_console(app);
    if (fd < 0) {
        files_set_status(app, "not connected");
        g_free(path);
        return;
    }
    static uint8_t buf[FS_SLOT_MAX];
    char           err[200];
    long           n = fs_get(fd, app->files_xfer_name, buf, sizeof(buf),
                              err, sizeof(err));
    files_resume_console(app);

    char st[400];
    if (n < 0) {
        snprintf(st, sizeof(st), "download failed: %s", err);
    } else {
        int hf = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (hf < 0) {
            snprintf(st, sizeof(st), "cannot write %s", path);
        } else {
            ssize_t w = write(hf, buf, (size_t)n);
            close(hf);
            snprintf(st, sizeof(st),
                     w == (ssize_t)n ? "downloaded %ld B -> %s"
                                     : "partial write (%ld B) -> %s",
                     n, path);
        }
    }
    files_set_status(app, st);
    g_free(path);
}

static void on_download(GtkButton *b, gpointer user)
{
    (void)b;
    App        *app = user;
    const char *sel = files_selected(app);
    if (!sel || name_is_dir(sel)) {
        files_set_status(app, "select a file to download");
        return;
    }
    files_child(app, sel, app->files_xfer_name, sizeof(app->files_xfer_name));

    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_initial_name(dlg, sel);
    gtk_file_dialog_save(dlg, GTK_WINDOW(app->files_win), NULL,
                         on_download_done, app);
    g_object_unref(dlg);
}

/* ------------------------------------------------------------------------- */
/* upload (host file -> device recv, into the current folder)                */
/* ------------------------------------------------------------------------- */

static void on_upload_done(GObject *src, GAsyncResult *res, gpointer user)
{
    App   *app  = user;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file) {
        return;
    }
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) {
        return;
    }
    gchar *data = NULL;
    gsize  len  = 0;
    char   st[400];
    if (!g_file_get_contents(path, &data, &len, NULL)) {
        files_set_status(app, "cannot read host file");
        g_free(path);
        return;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char child[300];
    files_child(app, base, child, sizeof(child));

    if (len > FS_SLOT_MAX) {
        snprintf(st, sizeof(st), "%s is %zu B > %d B (one slot)",
                 base, (size_t)len, FS_SLOT_MAX);
        files_set_status(app, st);
        g_free(data);
        g_free(path);
        return;
    }
    int fd = files_pause_console(app);
    if (fd < 0) {
        files_set_status(app, "not connected");
        g_free(data);
        g_free(path);
        return;
    }
    char err[200];
    int  rc = fs_put(fd, child, (const uint8_t *)data, (size_t)len,
                     err, sizeof(err));
    files_resume_console(app);

    files_refresh(app);
    if (rc == 0) {
        snprintf(st, sizeof(st), "uploaded %zu B -> /data/%s", (size_t)len, child);
    } else {
        snprintf(st, sizeof(st), "upload failed: %s", err);
    }
    files_set_status(app, st);
    g_free(data);
    g_free(path);
}

static void on_upload(GtkButton *b, gpointer user)
{
    (void)b;
    App           *app = user;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_open(dlg, GTK_WINDOW(app->files_win), NULL,
                         on_upload_done, app);
    g_object_unref(dlg);
}

/* ------------------------------------------------------------------------- */
/* new folder (mkdir, via a tiny entry dialog)                               */
/* ------------------------------------------------------------------------- */

static void on_newfolder_ok(GtkButton *btn, gpointer user)
{
    App         *app   = user;
    GtkWidget   *entry = g_object_get_data(G_OBJECT(btn), "entry");
    GtkWidget   *dlg   = g_object_get_data(G_OBJECT(btn), "dlg");
    const char  *name  = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (name && name[0] && !strchr(name, '/')) {
        char child[300];
        files_child(app, name, child, sizeof(child));
        int fd = files_pause_console(app);
        if (fd >= 0) {
            char out[160];
            fs_mkdir(fd, child, out, sizeof(out));
            files_resume_console(app);
        }
        files_refresh(app);
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
}

static void on_newfolder(GtkButton *b, gpointer user)
{
    (void)b;
    App *app = user;

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "New Folder");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(app->files_win));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 280, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_window_set_child(GTK_WINDOW(dlg), box);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "folder name");
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget *create = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(create, "suggested-action");
    g_object_set_data(G_OBJECT(create), "entry", entry);
    g_object_set_data(G_OBJECT(create), "dlg", dlg);
    g_signal_connect(create, "clicked", G_CALLBACK(on_newfolder_ok), app);
    g_signal_connect(entry, "activate", G_CALLBACK(on_newfolder_ok), app);
    /* the entry's "activate" handler needs the same data the button carries */
    g_object_set_data(G_OBJECT(entry), "entry", entry);
    g_object_set_data(G_OBJECT(entry), "dlg", dlg);
    gtk_box_append(GTK_BOX(box), create);

    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(entry);
}

/* ------------------------------------------------------------------------- */
/* window lifecycle                                                          */
/* ------------------------------------------------------------------------- */

static void on_files_destroy(GtkWidget *w, gpointer user)
{
    (void)w;
    App *app = user;
    app->files_win      = NULL;
    app->files_list     = NULL;
    app->files_usage    = NULL;
    app->files_status   = NULL;
    app->files_dl_btn   = NULL;
    app->files_del_btn  = NULL;
    app->files_path_lbl = NULL;
    app->files_up_btn   = NULL;
}

void files_window_open(App *app)
{
    if (app->files_win) {
        gtk_window_present(GTK_WINDOW(app->files_win));
        files_refresh(app);
        return;
    }
    app->files_cwd[0] = '\0';

    GtkWidget *win = gtk_window_new();
    app->files_win = win;
    gtk_window_set_title(GTK_WINDOW(win), "Files");
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 520);
    if (app->win) {
        gtk_window_set_transient_for(GTK_WINDOW(win), app->win);
    }
    g_signal_connect(win, "destroy", G_CALLBACK(on_files_destroy), app);

    /* header bar: path title + usage subtitle; Up/Refresh left, New/Upload right */
    GtkWidget *hb       = gtk_header_bar_new();
    GtkWidget *titlebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(titlebox, GTK_ALIGN_CENTER);
    app->files_path_lbl = gtk_label_new("/data");
    gtk_widget_add_css_class(app->files_path_lbl, "title");
    app->files_usage = gtk_label_new("the device file store");
    gtk_widget_add_css_class(app->files_usage, "subtitle");
    gtk_box_append(GTK_BOX(titlebox), app->files_path_lbl);
    gtk_box_append(GTK_BOX(titlebox), app->files_usage);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), titlebox);

    app->files_up_btn = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(app->files_up_btn, "Parent folder");
    gtk_widget_set_sensitive(app->files_up_btn, FALSE);
    g_signal_connect(app->files_up_btn, "clicked", G_CALLBACK(on_up), app);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), app->files_up_btn);

    GtkWidget *b_ref = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(b_ref, "Refresh");
    g_signal_connect(b_ref, "clicked", G_CALLBACK(on_refresh), app);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), b_ref);

    GtkWidget *b_up = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(b_up, "Upload a file here");
    g_signal_connect(b_up, "clicked", G_CALLBACK(on_upload), app);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), b_up);

    GtkWidget *b_new = gtk_button_new_from_icon_name("folder-new-symbolic");
    gtk_widget_set_tooltip_text(b_new, "New folder");
    g_signal_connect(b_new, "clicked", G_CALLBACK(on_newfolder), app);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), b_new);

    gtk_window_set_titlebar(GTK_WINDOW(win), hb);

    /* body: scrolled file list */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    app->files_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->files_list),
                                    GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(app->files_list, "navigation-sidebar");
    GtkWidget *ph = gtk_label_new("Empty folder");
    gtk_widget_add_css_class(ph, "dim-label");
    gtk_widget_set_margin_top(ph, 28);
    gtk_widget_set_margin_bottom(ph, 28);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(app->files_list), ph);
    g_signal_connect(app->files_list, "row-selected",
                     G_CALLBACK(on_row_selected), app);
    g_signal_connect(app->files_list, "row-activated",
                     G_CALLBACK(on_row_activated), app);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), app->files_list);
    gtk_box_append(GTK_BOX(box), sw);

    /* action bar: status + selection-gated Download / Delete */
    GtkWidget *ab = gtk_action_bar_new();
    app->files_status = gtk_label_new("");
    gtk_widget_add_css_class(app->files_status, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(app->files_status), PANGO_ELLIPSIZE_END);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(ab), app->files_status);

    app->files_del_btn = icon_label_button("user-trash-symbolic", "Delete");
    gtk_widget_add_css_class(app->files_del_btn, "destructive-action");
    gtk_widget_set_sensitive(app->files_del_btn, FALSE);
    g_signal_connect(app->files_del_btn, "clicked", G_CALLBACK(on_delete), app);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(ab), app->files_del_btn);

    app->files_dl_btn = icon_label_button("document-save-symbolic", "Download");
    gtk_widget_set_sensitive(app->files_dl_btn, FALSE);
    g_signal_connect(app->files_dl_btn, "clicked", G_CALLBACK(on_download), app);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(ab), app->files_dl_btn);

    gtk_box_append(GTK_BOX(box), ab);

    gtk_window_present(GTK_WINDOW(win));
    files_refresh(app);
}
