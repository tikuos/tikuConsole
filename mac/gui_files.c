/*
 * gui_files.c - a /data file-browser window for the TikuConsole GUI.
 *
 * Opened from the "Files..." button in the connection bar.  Presents the
 * device's /data store as a small file manager: a header bar (the store path
 * + live usage), an icon list of files, and a bottom action bar with
 * selection-gated Download / Delete plus a header Upload / Refresh.  All I/O
 * goes through the request/response helpers in fs.c (the C twin of tikufs.py);
 * each transfer pauses the console read-watch (files_pause/resume_console in
 * gui.c) so it owns the serial fd for the exchange.
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
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void files_set_status(App *app, const char *msg)
{
    if (app->files_status) {
        gtk_label_set_text(GTK_LABEL(app->files_status), msg);
    }
}

/* A button with a leading symbolic icon and a label. */
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

/* Selected filename (valid until the list is rebuilt), or NULL. */
static const char *files_selected(App *app)
{
    GtkListBoxRow *row =
        gtk_list_box_get_selected_row(GTK_LIST_BOX(app->files_list));
    if (!row) {
        return NULL;
    }
    GtkWidget *child = gtk_list_box_row_get_child(row);
    return child ? g_object_get_data(G_OBJECT(child), "fname") : NULL;
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

/* Distil "df" into a one-line usage summary for the header subtitle. */
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
    gtk_label_set_text(GTK_LABEL(app->files_usage), summary);
}

/* ------------------------------------------------------------------------- */
/* refresh (ls + df)                                                         */
/* ------------------------------------------------------------------------- */

static void files_refresh(App *app)
{
    int fd = files_pause_console(app);
    if (fd < 0) {
        gtk_label_set_text(GTK_LABEL(app->files_usage), "not connected");
        files_set_status(app, "Not connected -- press Connect first");
        files_clear_list(app);
        files_set_actions(app, FALSE);
        return;
    }
    char df[512], ls[8192];
    fs_df(fd, df, sizeof(df));
    fs_ls(fd, ls, sizeof(ls));
    files_resume_console(app);

    files_set_usage(app, df);
    files_clear_list(app);
    files_set_actions(app, FALSE);              /* rebuild clears the selection */

    const char *names[160];
    int n = fs_parse_names(ls, names, 160);     /* ls is mutated in place */
    for (int i = 0; i < n; i++) {
        GtkWidget *row  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 4);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);
        GtkWidget *icon = gtk_image_new_from_icon_name("text-x-generic-symbolic");
        GtkWidget *lbl  = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(row), icon);
        gtk_box_append(GTK_BOX(row), lbl);
        g_object_set_data_full(G_OBJECT(row), "fname",
                               g_strdup(names[i]), g_free);
        gtk_list_box_append(GTK_LIST_BOX(app->files_list), row);
    }
}

static void on_refresh(GtkButton *b, gpointer user)
{
    (void)b;
    files_refresh((App *)user);
}

/* ------------------------------------------------------------------------- */
/* selection                                                                 */
/* ------------------------------------------------------------------------- */

static void on_row_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer user)
{
    (void)lb;
    files_set_actions((App *)user, row != NULL);
}

/* forward decl: double-click / Enter downloads */
static void on_download(GtkButton *b, gpointer user);

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer user)
{
    (void)lb;
    (void)row;
    on_download(NULL, user);
}

/* ------------------------------------------------------------------------- */
/* delete                                                                    */
/* ------------------------------------------------------------------------- */

static void on_delete(GtkButton *b, gpointer user)
{
    (void)b;
    App        *app = user;
    const char *sel = files_selected(app);
    if (!sel) {
        files_set_status(app, "select a file to delete");
        return;
    }
    char name[128];
    snprintf(name, sizeof(name), "%s", sel);    /* copy: the refresh rebuilds */

    int fd = files_pause_console(app);
    if (fd < 0) {
        files_set_status(app, "not connected");
        return;
    }
    char out[256];
    fs_rm(fd, name, out, sizeof(out));
    files_resume_console(app);

    files_refresh(app);
    char st[300];
    if (strstr(out, "cannot")) {
        snprintf(st, sizeof(st), "delete failed: %s", out);
    } else {
        snprintf(st, sizeof(st), "deleted %s", name);
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
        return;                                 /* cancelled */
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
    if (!sel) {
        files_set_status(app, "select a file to download");
        return;
    }
    snprintf(app->files_xfer_name, sizeof(app->files_xfer_name), "%s", sel);

    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_initial_name(dlg, sel);
    gtk_file_dialog_save(dlg, GTK_WINDOW(app->files_win), NULL,
                         on_download_done, app);
    g_object_unref(dlg);
}

/* ------------------------------------------------------------------------- */
/* upload (host file -> device recv, via an Open dialog)                     */
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
    char name[128];
    snprintf(name, sizeof(name), "%s", base);

    if (len > FS_SLOT_MAX) {
        snprintf(st, sizeof(st), "%s is %zu B > %d B (one slot)",
                 name, (size_t)len, FS_SLOT_MAX);
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
    int  rc = fs_put(fd, name, (const uint8_t *)data, (size_t)len,
                     err, sizeof(err));
    files_resume_console(app);

    files_refresh(app);
    if (rc == 0) {
        snprintf(st, sizeof(st), "uploaded %zu B -> /data/%s", (size_t)len, name);
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
/* window lifecycle                                                          */
/* ------------------------------------------------------------------------- */

static void on_files_destroy(GtkWidget *w, gpointer user)
{
    (void)w;
    App *app = user;
    app->files_win     = NULL;
    app->files_list    = NULL;
    app->files_usage   = NULL;
    app->files_status  = NULL;
    app->files_dl_btn  = NULL;
    app->files_del_btn = NULL;
}

void files_window_open(App *app)
{
    if (app->files_win) {                       /* already open -> raise it */
        gtk_window_present(GTK_WINDOW(app->files_win));
        files_refresh(app);
        return;
    }

    GtkWidget *win = gtk_window_new();
    app->files_win = win;
    gtk_window_set_title(GTK_WINDOW(win), "Files");
    gtk_window_set_default_size(GTK_WINDOW(win), 440, 500);
    if (app->win) {
        gtk_window_set_transient_for(GTK_WINDOW(win), app->win);
    }
    g_signal_connect(win, "destroy", G_CALLBACK(on_files_destroy), app);

    /* --- header bar: path title + usage subtitle, Refresh / Upload --- */
    GtkWidget *hb       = gtk_header_bar_new();
    GtkWidget *titlebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(titlebox, GTK_ALIGN_CENTER);
    GtkWidget *t1 = gtk_label_new("/data");
    gtk_widget_add_css_class(t1, "title");
    app->files_usage = gtk_label_new("the device file store");
    gtk_widget_add_css_class(app->files_usage, "subtitle");
    gtk_box_append(GTK_BOX(titlebox), t1);
    gtk_box_append(GTK_BOX(titlebox), app->files_usage);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), titlebox);

    GtkWidget *b_ref = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(b_ref, "Refresh the listing");
    g_signal_connect(b_ref, "clicked", G_CALLBACK(on_refresh), app);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), b_ref);

    GtkWidget *b_up = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(b_up, "Upload a file to the device");
    g_signal_connect(b_up, "clicked", G_CALLBACK(on_upload), app);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), b_up);

    gtk_window_set_titlebar(GTK_WINDOW(win), hb);

    /* --- body: scrolled file list --- */
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
    GtkWidget *ph = gtk_label_new("No files in /data");
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

    /* --- action bar: status + selection-gated Download / Delete --- */
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
