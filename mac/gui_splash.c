/*
 * gui_splash.c - TikuConsole startup splash.
 *
 * Ports tcon/splash.py: a TikuBench-style splash (brand accent strip, TikuOS
 * logo, title, licence, a brief loading bar, the WEISER logo) shown for ~2 s,
 * then it presents the main window and destroys itself.  Logos load from the
 * repo's logo/ dir (next to the tikuConsole sources); a bold 'TikuOS' label
 * stands in if they are missing.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mach-o/dyld.h>

#include <gtk/gtk.h>

#include "gui.h"

/* Resolve <repo>/logo/<name> relative to the executable (mac/ -> ../logo).
 * Returns a g_strdup'd path if the file exists, else NULL. */
static char *logo_path(const char *name)
{
    char exe[2048];
    uint32_t sz = sizeof(exe);
    char *result = NULL;
    if (_NSGetExecutablePath(exe, &sz) == 0) {
        char *real = realpath(exe, NULL);
        if (real) {
            char *macdir = g_path_get_dirname(real);     /* .../tikuConsole/mac */
            char *parent = g_path_get_dirname(macdir);   /* .../tikuConsole */
            char *p = g_build_filename(parent, "logo", name, NULL);
            if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
                result = p;
            } else {
                g_free(p);
            }
            g_free(macdir);
            g_free(parent);
            free(real);
        }
    }
    return result;
}

static GtkWidget *logo_picture(const char *file, int w, int h)
{
    char *path = logo_path(file);
    if (!path) {
        return NULL;
    }
    GtkWidget *pic = gtk_picture_new_for_filename(path);
    g_free(path);
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request(pic, w, h);
    gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
    return pic;
}

static gboolean splash_tick(gpointer user)
{
    App *app = user;
    app->splash_ticks++;
    char load[16];
    snprintf(load, sizeof(load), "Loading%.*s", app->splash_ticks & 3, "...");
    gtk_label_set_text(GTK_LABEL(app->splash_load), load);
    double frac = app->splash_ticks / 16.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->splash_prog),
                                  frac > 1.0 ? 1.0 : frac);
    if (app->splash_ticks >= 16) {
        gtk_window_present(app->win);
        gtk_widget_grab_focus(GTK_WIDGET(app->cview));
        gtk_window_destroy(GTK_WINDOW(app->splash));
        app->splash = NULL;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void show_splash(App *app)
{
    GtkWidget *sp = gtk_application_window_new(app->app);
    gtk_window_set_decorated(GTK_WINDOW(sp), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(sp), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(sp), 460, 520);
    gtk_widget_add_css_class(sp, "splash");

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(sp), outer);

    GtkWidget *accent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(accent, "splash-accent");
    gtk_widget_set_size_request(accent, -1, 7);
    gtk_box_append(GTK_BOX(outer), accent);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(box, "splash-body");
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_box_append(GTK_BOX(outer), box);

    GtkWidget *tlogo = logo_picture("tikuos.jpeg", 140, 140);
    if (tlogo) {
        gtk_box_append(GTK_BOX(box), tlogo);
    } else {
        GtkWidget *art = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(art), "<span size='xx-large' "
                             "weight='bold' foreground='#14457f'>TikuOS</span>");
        gtk_box_append(GTK_BOX(box), art);
    }

    GtkWidget *title = gtk_label_new("TikuConsole");
    gtk_widget_add_css_class(title, "splash-title");
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *sub = gtk_label_new(NULL);
    gtk_widget_add_css_class(sub, "splash-sub");
    char subm[200];
    snprintf(subm, sizeof(subm), "serial console for <span foreground='#2e7d32'>"
             "<b>TikuOS</b></span> devices  \xc2\xb7  v%s", VERSION);
    gtk_label_set_markup(GTK_LABEL(sub), subm);
    gtk_box_append(GTK_BOX(box), sub);

    gtk_box_append(GTK_BOX(box), gtk_label_new(""));
    GtkWidget *au = gtk_label_new("Ambuj Varshney");
    gtk_widget_add_css_class(au, "splash-author");
    gtk_box_append(GTK_BOX(box), au);
    GtkWidget *lic = gtk_label_new("\xc2\xa9 TikuOS \xc2\xb7 Licensed under "
                                   "Apache-2.0");
    gtk_widget_add_css_class(lic, "splash-lic");
    gtk_box_append(GTK_BOX(box), lic);
    gtk_box_append(GTK_BOX(box), gtk_label_new(""));

    GtkWidget *lrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(lrow, GTK_ALIGN_CENTER);
    GtkWidget *spin = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spin));
    gtk_box_append(GTK_BOX(lrow), spin);
    app->splash_load = gtk_label_new("Loading");
    gtk_widget_add_css_class(app->splash_load, "splash-load");
    gtk_box_append(GTK_BOX(lrow), app->splash_load);
    gtk_box_append(GTK_BOX(box), lrow);

    app->splash_prog = gtk_progress_bar_new();
    gtk_widget_set_size_request(app->splash_prog, 300, -1);
    gtk_widget_set_margin_top(app->splash_prog, 4);
    gtk_box_append(GTK_BOX(box), app->splash_prog);

    GtkWidget *wlogo = logo_picture("weiser.png", 170, 96);
    if (wlogo) {
        gtk_widget_set_margin_top(wlogo, 10);
        gtk_box_append(GTK_BOX(box), wlogo);
    }

    app->splash = sp;
    app->splash_ticks = 0;
    gtk_window_present(GTK_WINDOW(sp));
    g_timeout_add(140, splash_tick, app);
}
