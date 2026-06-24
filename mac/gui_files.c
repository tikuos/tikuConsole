/*
 * gui_files.c - a /data file-browser window for the TikuConsole GUI.
 *
 * Opened from the "Files..." button in the connection bar.  Lists the device's
 * /data store (ls + df), and downloads / uploads / deletes files over the
 * shell console using the request/response helpers in fs.c.  Each transfer
 * pauses the console read-watch for its duration (files_pause/resume_console
 * in gui.c) so it owns the serial fd, exactly like the host tool tikufs.py.
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

/* Selected filename (valid until the list is rebuilt), or NULL. */
static const char *files_selected(App *app)
{
    GtkListBoxRow *row =
        gtk_list_box_get_selected_row(GTK_LIST_BOX(app->files_list));
    if (!row) {
        return NULL;
    }
    GtkWidget *child = gtk_list_box_row_get_child(row);
    return child ? gtk_label_get_text(GTK_LABEL(child)) : NULL;
}

static void files_clear_list(App *app)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(app->files_list)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(app->files_list), c);
    }
}

/* ------------------------------------------------------------------------- */
/* refresh (ls + df)                                                         */
/* ------------------------------------------------------------------------- */

static void files_refresh(App *app)
{
    int fd = files_pause_console(app);
    if (fd < 0) {
        files_set_status(app, "not connected -- press Connect first");
        return;
    }
    char df[512], ls[8192];
    fs_df(fd, df, sizeof(df));
    fs_ls(fd, ls, sizeof(ls));
    files_resume_console(app);

    gtk_label_set_text(GTK_LABEL(app->files_usage), df[0] ? df : "(no usage)");

    files_clear_list(app);
    const char *names[160];
    int n = fs_parse_names(ls, names, 160);     /* mutates ls in place */
    for (int i = 0; i < n; i++) {
        GtkWidget *lbl = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_widget_set_margin_start(lbl, 4);
        gtk_widget_set_margin_end(lbl, 4);
        gtk_list_box_append(GTK_LIST_BOX(app->files_list), lbl);
    }
    char st[64];
    snprintf(st, sizeof(st), "%d file%s in /data", n, n == 1 ? "" : "s");
    files_set_status(app, st);
}

static void on_refresh(GtkButton *b, gpointer user)
{
    (void)b;
    files_refresh((App *)user);
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

    char st[300];
    if (strstr(out, "cannot")) {
        snprintf(st, sizeof(st), "delete failed: %s", out);
    } else {
        snprintf(st, sizeof(st), "deleted %s", name);
    }
    files_set_status(app, st);
    files_refresh(app);
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

    if (rc == 0) {
        snprintf(st, sizeof(st), "uploaded %zu B -> /data/%s", (size_t)len, name);
    } else {
        snprintf(st, sizeof(st), "upload failed: %s", err);
    }
    files_set_status(app, st);
    g_free(data);
    g_free(path);
    files_refresh(app);
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
    app->files_win    = NULL;
    app->files_list   = NULL;
    app->files_usage  = NULL;
    app->files_status = NULL;
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
    gtk_window_set_title(GTK_WINDOW(win), "TikuOS /data");
    gtk_window_set_default_size(GTK_WINDOW(win), 380, 440);
    if (app->win) {
        gtk_window_set_transient_for(GTK_WINDOW(win), app->win);
    }
    g_signal_connect(win, "destroy", G_CALLBACK(on_files_destroy), app);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_window_set_child(GTK_WINDOW(win), box);

    app->files_usage = gtk_label_new("press Refresh to list /data");
    gtk_label_set_xalign(GTK_LABEL(app->files_usage), 0);
    gtk_label_set_selectable(GTK_LABEL(app->files_usage), TRUE);
    gtk_widget_add_css_class(app->files_usage, "dim-label");
    gtk_box_append(GTK_BOX(box), app->files_usage);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    app->files_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->files_list),
                                    GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), app->files_list);
    gtk_box_append(GTK_BOX(box), sw);

    GtkWidget *row   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b_ref = gtk_button_new_with_label("Refresh");
    GtkWidget *b_get = gtk_button_new_with_label("Download");
    GtkWidget *b_put = gtk_button_new_with_label("Upload");
    GtkWidget *b_del = gtk_button_new_with_label("Delete");
    gtk_widget_set_hexpand(b_ref, TRUE);
    gtk_widget_set_hexpand(b_get, TRUE);
    gtk_widget_set_hexpand(b_put, TRUE);
    gtk_widget_set_hexpand(b_del, TRUE);
    g_signal_connect(b_ref, "clicked", G_CALLBACK(on_refresh),  app);
    g_signal_connect(b_get, "clicked", G_CALLBACK(on_download), app);
    g_signal_connect(b_put, "clicked", G_CALLBACK(on_upload),   app);
    g_signal_connect(b_del, "clicked", G_CALLBACK(on_delete),   app);
    gtk_box_append(GTK_BOX(row), b_ref);
    gtk_box_append(GTK_BOX(row), b_get);
    gtk_box_append(GTK_BOX(row), b_put);
    gtk_box_append(GTK_BOX(row), b_del);
    gtk_box_append(GTK_BOX(box), row);

    app->files_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->files_status), 0);
    gtk_label_set_wrap(GTK_LABEL(app->files_status), TRUE);
    gtk_label_set_selectable(GTK_LABEL(app->files_status), TRUE);
    gtk_box_append(GTK_BOX(box), app->files_status);

    gtk_window_present(GTK_WINDOW(win));
    files_refresh(app);
}
