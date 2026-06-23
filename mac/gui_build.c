/*
 * gui_build.c - TikuConsole firmware build/flash bar.
 *
 * Ports tcon/build.py: a "Firmware" bar above the connection bar -- pick an
 * MCU, flip the feature checkboxes (shell / networking / BASIC / colour), and
 * hit Build & Flash.  It runs `make clean` -> `make <flags>` -> `make flash
 * <flags>` in the tikuOS root, streaming the output live into the console
 * (GSubprocess, so the GTK loop stays responsive), and on a clean flash auto-
 * connects to the freshly programmed board.
 *
 * The board table mirrors TikuBench's tikubench.core.board.BOARDS (one source
 * of truth for MCU names + default baud); the Makefile's own `flash` target
 * handles each family's flash path (eZ-FET / BOOTSEL / J-Link), so this only
 * has to drive make.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach-o/dyld.h>

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "gui.h"

/* Board profiles -- mirrors TikuBench's BOARDS, plus friendly display names for
 * the grouped picker.  Kept grouped by family (contiguous) so the bar can frame
 * each subcategory in one pass. */
typedef struct {
    const char *key;       /* TikuBench board key / make MCU= value */
    const char *mcu;
    const char *family;    /* "msp430" | "rp2350" | "ambiq" */
    int         baud;
    const char *fam_disp;  /* subcategory header, e.g. "MSP430" */
    const char *var_disp;  /* variant radio label, e.g. "FR5994" */
} board_t;

static const board_t BOARDS[] = {
    {"msp430fr5969", "msp430fr5969", "msp430", 9600,   "MSP430",       "FR5969"},
    {"msp430fr5994", "msp430fr5994", "msp430", 9600,   "MSP430",       "FR5994"},
    {"msp430fr6989", "msp430fr6989", "msp430", 9600,   "MSP430",       "FR6989"},
    {"apollo510",    "apollo510",    "ambiq",  115200, "Apollo",       "Apollo510"},
    {"apollo4l",     "apollo4l",     "ambiq",  115200, "Apollo",       "Apollo4 Lite"},
    {"rp2350",       "rp2350",       "rp2350", 115200, "Raspberry Pi", "RP2350"},
};
#define N_BOARDS ((int)(sizeof(BOARDS) / sizeof(BOARDS[0])))

static gboolean resolve_proj_dir(App *app);
static void bld_run_step(App *app);
static void bld_on_exit(GObject *src, GAsyncResult *res, gpointer user);
static void bld_done(App *app, gboolean ok);
static gboolean bld_autoconnect(gpointer user);
static void on_build_flash(GtkButton *btn, gpointer user);

/* ------------------------------------------------------------------------- */
/* Locate the tikuOS root (where make runs)                                  */
/* ------------------------------------------------------------------------- */

static gboolean is_root_dir(const char *d)
{
    char *mk = g_build_filename(d, "Makefile", NULL);
    char *kn = g_build_filename(d, "kernel", NULL);
    gboolean ok = g_file_test(mk, G_FILE_TEST_IS_REGULAR) &&
                  g_file_test(kn, G_FILE_TEST_IS_DIR);
    g_free(mk);
    g_free(kn);
    return ok;
}

static gboolean resolve_proj_dir(App *app)
{
    if (app->proj_dir[0]) {
        return TRUE;
    }
    const char *env = g_getenv("TIKUOS_DIR");
    if (!env || !*env) {
        env = g_getenv("PROJ_DIR");
    }
    if (env && *env && is_root_dir(env)) {
        g_strlcpy(app->proj_dir, env, sizeof(app->proj_dir));
        return TRUE;
    }
    /* Walk up from the executable (then cwd) for a dir with Makefile + kernel/.
     * The Makefile+kernel marker skips mac/ (which has only a Makefile). */
    char start[2048] = {0};
    char exe[2048];
    uint32_t sz = sizeof(exe);
    if (_NSGetExecutablePath(exe, &sz) == 0) {
        char *real = realpath(exe, NULL);
        if (real) {
            char *d = g_path_get_dirname(real);
            g_strlcpy(start, d, sizeof(start));
            g_free(d);
            free(real);
        }
    }
    if (!start[0] && !getcwd(start, sizeof(start))) {
        start[0] = '\0';
    }
    char cur[2048];
    g_strlcpy(cur, start, sizeof(cur));
    for (int i = 0; i < 8 && cur[0]; i++) {
        if (is_root_dir(cur)) {
            g_strlcpy(app->proj_dir, cur, sizeof(app->proj_dir));
            return TRUE;
        }
        char *parent = g_path_get_dirname(cur);
        if (!parent || strcmp(parent, cur) == 0) {
            g_free(parent);
            break;
        }
        g_strlcpy(cur, parent, sizeof(cur));
        g_free(parent);
    }
    return app->proj_dir[0] != '\0';
}

/* ------------------------------------------------------------------------- */
/* Feature checkboxes -> make flags                                          */
/* ------------------------------------------------------------------------- */

static void bld_free_flags(App *app)
{
    for (int i = 0; i < app->bld_nflag; i++) {
        g_free(app->bld_flagv[i]);
        app->bld_flagv[i] = NULL;
    }
    app->bld_nflag = 0;
}

#define BLD_ADD(app, s) \
    do { if ((app)->bld_nflag < 38) \
            (app)->bld_flagv[(app)->bld_nflag++] = g_strdup(s); } while (0)

/* Translate the MCU + feature checkboxes into make variables.  Features whose
 * Makefile default can be ON -- shell (always) and BASIC (defaults ON on
 * ambiq) -- are emitted explicitly as =0/1 so an unchecked box actively turns
 * the feature off rather than letting the platform default win. */
static void bld_build_flags(App *app, const board_t *b)
{
    bld_free_flags(app);
    gboolean shell = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->bld_shell));
    gboolean net   = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->bld_net));
    gboolean basic = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->bld_basic));
    gboolean color = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->bld_color));

    BLD_ADD(app, "HAS_TESTS=0");
    BLD_ADD(app, "HAS_EXAMPLES=0");
    BLD_ADD(app, shell ? "TIKU_SHELL_ENABLE=1" : "TIKU_SHELL_ENABLE=0");
    BLD_ADD(app, basic ? "TIKU_SHELL_BASIC_ENABLE=1" : "TIKU_SHELL_BASIC_ENABLE=0");
    BLD_ADD(app, color ? "TIKU_SHELL_COLOR=1" : "TIKU_SHELL_COLOR=0");
    if (net) {
        BLD_ADD(app, "TIKU_KIT_NET_ENABLE=1");
        BLD_ADD(app, "TIKU_SHELL_NET_TEST=1");
    }
    if (basic && strcmp(b->family, "msp430") == 0) {
        BLD_ADD(app, "MEMORY_MODEL=large");   /* BASIC needs it on MSP430 */
    }
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "MCU=%s", b->mcu);
    BLD_ADD(app, tmp);
    snprintf(tmp, sizeof(tmp), "UART_BAUD=%d", b->baud);
    BLD_ADD(app, tmp);
}

static void bld_profile(App *app, char *out, size_t len)
{
    const struct { GtkWidget *w; const char *n; } F[] = {
        {app->bld_shell, "shell"}, {app->bld_net, "net"},
        {app->bld_basic, "BASIC"}, {app->bld_color, "colour"},
    };
    out[0] = '\0';
    int any = 0;
    for (int i = 0; i < 4; i++) {
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(F[i].w))) {
            if (any) {
                g_strlcat(out, "+", len);
            }
            g_strlcat(out, F[i].n, len);
            any = 1;
        }
    }
    if (!any) {
        g_strlcpy(out, "bare", len);
    }
}

/* ------------------------------------------------------------------------- */
/* Build + flash, streamed into the console                                  */
/* ------------------------------------------------------------------------- */

static void bld_on_chunk(GObject *src, GAsyncResult *res, gpointer user)
{
    App *app = user;
    GInputStream *st = G_INPUT_STREAM(src);
    GBytes *data = g_input_stream_read_bytes_finish(st, res, NULL);
    if (data) {
        gsize n = 0;
        const char *p = g_bytes_get_data(data, &n);
        if (n > 0) {
            /* Feed raw bytes to the ANSI-aware console (colour + CR/BS, just
             * like live serial output). */
            console_append(app, p, (int)n);
            g_bytes_unref(data);
            g_input_stream_read_bytes_async(st, 4096, G_PRIORITY_DEFAULT, NULL,
                                            bld_on_chunk, app);
            return;
        }
        g_bytes_unref(data);
    }
    if (app->bld_proc) {                       /* EOF -> reap + status */
        g_subprocess_wait_async(app->bld_proc, NULL, bld_on_exit, app);
    }
}

static void bld_on_exit(GObject *src, GAsyncResult *res, gpointer user)
{
    App *app = user;
    GSubprocess *proc = G_SUBPROCESS(src);
    gboolean ok = g_subprocess_wait_finish(proc, res, NULL) &&
                  g_subprocess_get_successful(proc);
    g_clear_object(&app->bld_proc);
    if (!ok) {
        bld_done(app, FALSE);
        return;
    }
    app->bld_step++;                           /* build -> flash -> end */
    bld_run_step(app);
}

static void bld_run_step(App *app)
{
    if (app->bld_step > 2) {
        bld_done(app, TRUE);
        return;
    }
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup("make"));
    if (app->bld_step == 0) {
        g_ptr_array_add(a, g_strdup("clean"));
    } else if (app->bld_step == 2) {
        g_ptr_array_add(a, g_strdup("flash"));
    }
    if (app->bld_step != 0) {
        for (int i = 0; i < app->bld_nflag; i++) {
            g_ptr_array_add(a, g_strdup(app->bld_flagv[i]));
        }
    }
    GString *ls = g_string_new(NULL);
    for (guint i = 0; i < a->len; i++) {
        if (i) {
            g_string_append_c(ls, ' ');
        }
        g_string_append(ls, (char *)g_ptr_array_index(a, i));
    }
    char *line = g_strdup_printf("\x1b[1;30m$ %s\x1b[0m\n", ls->str);
    console_append(app, line, -1);
    g_free(line);
    g_string_free(ls, TRUE);
    g_ptr_array_add(a, NULL);                  /* NULL-terminate argv */

    GError *err = NULL;
    GSubprocessLauncher *L = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_set_cwd(L, app->proj_dir);
    GSubprocess *proc = g_subprocess_launcher_spawnv(
        L, (const gchar * const *)a->pdata, &err);
    g_object_unref(L);
    g_ptr_array_free(a, TRUE);
    if (!proc) {
        char *m = g_strdup_printf("\x1b[1;31mspawn failed: %s\x1b[0m\n",
                                  err ? err->message : "?");
        console_append(app, m, -1);
        g_free(m);
        if (err) {
            g_error_free(err);
        }
        bld_done(app, FALSE);
        return;
    }
    app->bld_proc = proc;
    GInputStream *st = g_subprocess_get_stdout_pipe(proc);
    g_input_stream_read_bytes_async(st, 4096, G_PRIORITY_DEFAULT, NULL,
                                    bld_on_chunk, app);
}

static void bld_done(App *app, gboolean ok)
{
    app->bld_running = FALSE;
    bld_free_flags(app);
    gtk_widget_set_sensitive(app->bld_btn, TRUE);
    gtk_button_set_label(GTK_BUTTON(app->bld_btn), "Build & Flash");
    if (!ok) {
        console_append(app, "\x1b[1;31m\xe2\x94\x80\xe2\x94\x80 build/flash "
                       "FAILED -- see output above \xe2\x94\x80\xe2\x94\x80"
                       "\x1b[0m\n", -1);
        set_status(app, "build/flash failed", TRUE);
        return;
    }
    console_append(app, "\x1b[1;32m\xe2\x94\x80\xe2\x94\x80 build + flash OK "
                   "-- connecting\xe2\x80\xa6 \xe2\x94\x80\xe2\x94\x80\x1b[0m\n",
                   -1);
    /* Match the console baud to the flashed board, then auto-connect.  The
     * board reboots after `make flash` and may re-enumerate, so poll for the
     * port instead of a single shot. */
    char b[16];
    snprintf(b, sizeof(b), "%d", app->bld_board_baud);
    gtk_editable_set_text(GTK_EDITABLE(app->baud), b);
    char st[140];
    snprintf(st, sizeof(st), "flashed %s -- waiting for the port\xe2\x80\xa6",
             app->bld_board_key);
    set_status(app, st, FALSE);
    app->bld_try = 0;
    g_timeout_add(1000, bld_autoconnect, app);
}

static gboolean bld_autoconnect(gpointer user)
{
    App *app = user;
    if (gui_autoconnect_step(app)) {
        return G_SOURCE_REMOVE;
    }
    if (++app->bld_try >= 10) {                /* ~10 s, then hand over */
        console_append(app, "\x1b[1;33m[tikuconsole] port not ready -- press "
                       "Connect when the board re-appears.\x1b[0m\n", -1);
        set_status(app, "flashed -- press Connect when the port appears", TRUE);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;                   /* retry in 1 s */
}

static void on_build_flash(GtkButton *btn, gpointer user)
{
    (void)btn;
    App *app = user;
    if (app->bld_running) {
        return;
    }
    if (!resolve_proj_dir(app)) {
        set_status(app, "tikuOS root not found -- set TIKUOS_DIR to the repo "
                        "root", TRUE);
        return;
    }
    int idx = -1;
    for (int i = 0; i < app->bld_nradios; i++) {
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(app->bld_radios[i]))) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        set_status(app, "pick an MCU first", TRUE);
        return;
    }
    const board_t *b = &BOARDS[idx];
    bld_build_flags(app, b);
    char profile[80];
    bld_profile(app, profile, sizeof(profile));

    /* Flashing drives the same USB debugger the console holds open; free it. */
    gui_disconnect(app);

    g_strlcpy(app->bld_board_key, b->key, sizeof(app->bld_board_key));
    app->bld_board_baud = b->baud;
    app->bld_running = TRUE;
    app->bld_step = 0;
    gtk_widget_set_sensitive(app->bld_btn, FALSE);
    gtk_button_set_label(GTK_BUTTON(app->bld_btn), "Building\xe2\x80\xa6");
    gtk_widget_remove_css_class(GTK_WIDGET(app->cview), "console-off");
    char st[160];
    snprintf(st, sizeof(st), "building %s (%s)\xe2\x80\xa6", b->key, profile);
    set_status(app, st, FALSE);
    char *hdr = g_strdup_printf("\n\x1b[1;34m\xe2\x94\x80\xe2\x94\x80 build & "
                                "flash: %s \xc2\xb7 %s \xe2\x94\x80\xe2\x94\x80"
                                "\x1b[0m\n", b->key, profile);
    console_append(app, hdr, -1);
    g_free(hdr);
    bld_run_step(app);
}

/* ------------------------------------------------------------------------- */
/* Bar assembly                                                              */
/* ------------------------------------------------------------------------- */

/* A user toggling an MCU stops auto-select from overriding their choice. */
static void on_mcu_toggled(GtkCheckButton *rb, gpointer user)
{
    App *app = user;
    if (app->bld_set_programmatic) {
        return;                                /* our own set_active, not a click */
    }
    if (gtk_check_button_get_active(rb)) {
        app->bld_user_picked = TRUE;
    }
}

/* Auto-select the MCU family of a currently-attached board.  Detection is
 * family-level only: the variants within a family share a USB id (eZ-FET for
 * the MSP430s, the J-Link VCOM for both Apollos), so it selects the family's
 * default variant and leaves the exact pick to the now-grouped radios.  No-ops
 * once the user has chosen, and when nothing is attached (keeps the current
 * selection). */
void bld_autoselect(App *app)
{
    if (app->bld_user_picked || app->bld_nradios == 0) {
        return;
    }
    port_info_t p[PORTS_MAX];
    int n = ports_scan(p, PORTS_MAX);
    int idx = -1;
    for (int i = 0; i < n && idx < 0; i++) {
        char plat[64];
        g_strlcpy(plat, p[i].label, sizeof(plat));
        for (char *q = plat; *q; q++) {
            *q = (char)g_ascii_tolower(*q);
        }
        const char *key = NULL;
        if (strstr(plat, "apollo")) {
            key = "apollo510";                 /* family default (510 vs 4 Lite) */
        } else if (strstr(plat, "rp2") || strstr(plat, "pico")) {
            key = "rp2350";
        } else if (strstr(plat, "msp")) {
            key = "msp430fr5994";
        }
        if (key) {
            for (int j = 0; j < N_BOARDS; j++) {
                if (strcmp(BOARDS[j].key, key) == 0) {
                    idx = j;
                    break;
                }
            }
        }
    }
    if (idx < 0) {
        return;                                /* nothing attached -> keep pick */
    }
    app->bld_set_programmatic = TRUE;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->bld_radios[idx]), TRUE);
    app->bld_set_programmatic = FALSE;
}

GtkWidget *build_buildbar(App *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    /* Row 1: Microcontroller, grouped by family into framed subcategories.
     * One shared radio group across all families -> exactly one board total. */
    GtkWidget *mrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mlbl = gtk_label_new("Microcontroller");
    gtk_widget_set_valign(mlbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(mrow), mlbl);
    app->bld_nradios = 0;
    GtkWidget *group = NULL;
    GtkWidget *fam_box = NULL;
    const char *cur_fam = NULL;
    for (int i = 0; i < N_BOARDS; i++) {
        if (cur_fam == NULL || strcmp(cur_fam, BOARDS[i].fam_disp) != 0) {
            GtkWidget *frame = gtk_frame_new(BOARDS[i].fam_disp);
            gtk_widget_set_valign(frame, GTK_ALIGN_START);
            fam_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            gtk_widget_set_margin_start(fam_box, 6);
            gtk_widget_set_margin_end(fam_box, 6);
            gtk_widget_set_margin_top(fam_box, 2);
            gtk_widget_set_margin_bottom(fam_box, 4);
            gtk_frame_set_child(GTK_FRAME(frame), fam_box);
            gtk_box_append(GTK_BOX(mrow), frame);
            cur_fam = BOARDS[i].fam_disp;
        }
        GtkWidget *rb = gtk_check_button_new_with_label(BOARDS[i].var_disp);
        if (group == NULL) {                   /* CheckButton + group = radio */
            group = rb;
        } else {
            gtk_check_button_set_group(GTK_CHECK_BUTTON(rb),
                                       GTK_CHECK_BUTTON(group));
        }
        char tip[96];
        snprintf(tip, sizeof(tip), "make MCU=%s \xc2\xb7 %d baud",
                 BOARDS[i].mcu, BOARDS[i].baud);
        gtk_widget_set_tooltip_text(rb, tip);
        g_signal_connect(rb, "toggled", G_CALLBACK(on_mcu_toggled), app);
        app->bld_radios[app->bld_nradios++] = rb;
        gtk_box_append(GTK_BOX(fam_box), rb);
    }
    gtk_box_append(GTK_BOX(box), mrow);

    /* Row 2: feature checkboxes + the Build & Flash button. */
    GtkWidget *frow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(frow), gtk_label_new("Build"));
    app->bld_shell = gtk_check_button_new_with_label("shell");
    app->bld_net = gtk_check_button_new_with_label("networking");
    app->bld_basic = gtk_check_button_new_with_label("BASIC");
    app->bld_color = gtk_check_button_new_with_label("colour");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->bld_shell), TRUE);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->bld_net), TRUE);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->bld_color), TRUE);
    gtk_widget_set_tooltip_text(app->bld_shell,
        "interactive shell (TIKU_SHELL_ENABLE=1)");
    gtk_widget_set_tooltip_text(app->bld_net,
        "SLIP/IP + telnet/CoAP/MQTT (TIKU_SHELL_NET_TEST=1)");
    gtk_widget_set_tooltip_text(app->bld_basic,
        "Tiku BASIC interpreter (TIKU_SHELL_BASIC_ENABLE=1)");
    gtk_widget_set_tooltip_text(app->bld_color,
        "ANSI colour in the shell (TIKU_SHELL_COLOR=1)");
    gtk_box_append(GTK_BOX(frow), app->bld_shell);
    gtk_box_append(GTK_BOX(frow), app->bld_net);
    gtk_box_append(GTK_BOX(frow), app->bld_basic);
    gtk_box_append(GTK_BOX(frow), app->bld_color);

    app->bld_btn = gtk_button_new_with_label("Build & Flash");
    gtk_widget_add_css_class(app->bld_btn, "suggested-action");
    gtk_widget_set_hexpand(app->bld_btn, TRUE);
    gtk_widget_set_halign(app->bld_btn, GTK_ALIGN_END);
    g_signal_connect(app->bld_btn, "clicked", G_CALLBACK(on_build_flash), app);
    gtk_box_append(GTK_BOX(frow), app->bld_btn);
    gtk_box_append(GTK_BOX(box), frow);

    /* A baseline pick (so one is always selected), then auto-detect the
     * attached board's family on top -- both programmatic, so neither counts
     * as the user choosing. */
    app->bld_set_programmatic = TRUE;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->bld_radios[0]), TRUE);
    app->bld_set_programmatic = FALSE;
    bld_autoselect(app);
    return box;
}

/* Smoke aid: print the resolved root + the make flags for the current
 * selection, so the build wiring can be checked without a board. */
void bld_debug_dump(App *app)
{
    resolve_proj_dir(app);
    fprintf(stderr, "[tikuconsole-dump] proj_dir = %s\n",
            app->proj_dir[0] ? app->proj_dir : "(NOT FOUND)");
    int idx = 0;
    for (int i = 0; i < app->bld_nradios; i++) {
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(app->bld_radios[i]))) {
            idx = i;
            break;
        }
    }
    bld_build_flags(app, &BOARDS[idx]);
    GString *s = g_string_new(NULL);
    for (int i = 0; i < app->bld_nflag; i++) {
        if (i) {
            g_string_append_c(s, ' ');
        }
        g_string_append(s, app->bld_flagv[i]);
    }
    fprintf(stderr, "[tikuconsole-dump] board=%s flags: make %s\n",
            BOARDS[idx].key, s->str);
    g_string_free(s, TRUE);
    bld_free_flags(app);
}
