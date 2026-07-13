/*
 * gui.c - TikuConsole, the GTK4 serial console for TikuOS (macOS).
 *
 * The C/GTK4 twin of the Linux Python tcon/ app.  A serial console that behaves
 * like a terminal: a window-level key controller forwards keystrokes to the
 * board, an ANSI/SGR decoder colours the output, and the same wire is always
 * SLIP-demuxed so IP frames are separated from console text.  It wraps the
 * shared bridge core (bridge.c).
 *
 * This file owns the window, the console, the keyboard, the serial link and the
 * port picker.  The networking side-panel (utun bridge, UDP relay, NAT) lives
 * in gui_net.c and the in-app pinger in gui_ping.c -- all sharing one App.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/dyld.h>          /* _NSGetExecutablePath (window-icon path) */

#include <glib-unix.h>
#include <gtk/gtk.h>

#include "bridge.h"
#include "gui.h"
#include "ports.h"
#include "ble_mac.h"

static void teardown(App *app);

/* ------------------------------------------------------------------------- */
/* Small helpers (shared across modules -- see gui.h)                        */
/* ------------------------------------------------------------------------- */

void ser_write(App *app, const char *buf, size_t len)
{
    if (app->ser_fd < 0) {
        return;
    }
    /* The serial fd is non-blocking (bridge.c), so a single write() can
     * short-count or return EAGAIN when the USB-serial TX buffer fills during a
     * burst -- e.g. a multi-segment TLS flight forwarded over SLIP. The old code
     * dropped the unwritten tail, truncating SLIP frames mid-flight (corrupt TLS
     * records / stalled handshakes). Drain the whole buffer like the file-xfer
     * path (fs.c) does, honouring EAGAIN, with a no-progress deadline so a
     * stalled board can't wedge the UI. (pyserial's blocking write does this.) */
    size_t off = 0;
    gint64 t0 = g_get_monotonic_time();
    while (off < len) {
        ssize_t w = write(app->ser_fd, buf + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            t0 = g_get_monotonic_time();          /* progress: reset stall clock */
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (g_get_monotonic_time() - t0 > 2 * G_USEC_PER_SEC) {
                break;                            /* no drain for 2s: give up */
            }
            usleep(1000);
        } else {
            break;                                /* hard error (e.g. unplugged) */
        }
    }
}

void send_line(App *app, const char *line)
{
    ser_write(app, line, strlen(line));
    ser_write(app, "\r", 1);
}

void set_status(App *app, const char *text, gboolean err)
{
    char buf[600];
    if (err && strstr(text, "error") == NULL) {
        snprintf(buf, sizeof(buf), "error: %s", text);
    } else {
        snprintf(buf, sizeof(buf), "%s", text);
    }
    gtk_label_set_text(GTK_LABEL(app->status), buf);
}

static void set_led(GtkWidget *lbl, gboolean on, const char *text)
{
    char m[128];
    snprintf(m, sizeof(m), "<span foreground='%s'>\xe2\x97\x8f</span> %s",
             on ? GREEN : "#ff6b6b", text);
    gtk_label_set_markup(GTK_LABEL(lbl), m);
}

void update_leds(App *app)
{
    set_led(app->usb_led, app->ser_fd >= 0, "USB");
    set_led(app->slip_led, app->slip_on, "SLIP");
    set_led(app->nat_led, app->nat_on, "Internet");
    wifi_update_led(app);
}

/* The connection-bar baud is a dropdown of standard rates (ports tcon's
 * BaudPicker). These two shims keep the old string-based call sites working. */
static const char *const BAUD_RATES[] = {
    "9600", "19200", "38400", "57600",
    "115200", "230400", "460800", "921600", NULL
};
#define BAUD_N        8       /* entries before the NULL terminator */
#define BAUD_DEFAULT  4       /* index of "115200" */

const char *baud_get_text(App *app)
{
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->baud));
    return (i < BAUD_N) ? BAUD_RATES[i] : BAUD_RATES[BAUD_DEFAULT];
}

void baud_set_text(App *app, const char *s)
{
    for (guint i = 0; i < BAUD_N; i++) {
        if (strcmp(BAUD_RATES[i], s) == 0) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(app->baud), i);
            return;
        }
    }
}

static void set_slip_led(App *app, gboolean on)
{
    if (on != app->slip_on) {           /* driven by the board's own messages */
        app->slip_on = on;
        update_leds(app);
    }
}

/* ------------------------------------------------------------------------- */
/* Console: ANSI/SGR colour, BS/CR handling, auto-follow                     */
/* ------------------------------------------------------------------------- */

static void led_scan(App *app, const char *text, int len)
{
    g_string_append_len(app->slip_scan, text, len);
    if (app->slip_scan->len > 96) {
        g_string_erase(app->slip_scan, 0, (gssize)(app->slip_scan->len - 96));
    }
    if (strstr(app->slip_scan->str, "SLIP off --")) {
        set_slip_led(app, FALSE);
        g_string_set_size(app->slip_scan, 0);
    } else if (strstr(app->slip_scan->str, "SLIP on.")) {
        set_slip_led(app, TRUE);
        g_string_set_size(app->slip_scan, 0);
    }
}

static void console_raw_insert(App *app, const char *t, int len)
{
    if (len <= 0) {
        return;
    }
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app->cbuf, &end);
    int off = gtk_text_iter_get_offset(&end);
    gtk_text_buffer_insert(app->cbuf, &end, t, len);

    GtkTextIter s2, e2;
    gtk_text_buffer_get_iter_at_offset(app->cbuf, &s2, off);
    gtk_text_buffer_get_end_iter(app->cbuf, &e2);
    if (app->sgr_bold) {
        gtk_text_buffer_apply_tag(app->cbuf, app->tag_bold, &s2, &e2);
    }
    if (app->sgr_dim) {
        gtk_text_buffer_apply_tag(app->cbuf, app->tag_dim, &s2, &e2);
    }
    if (app->sgr_fg >= 30 && app->sgr_fg <= 37) {
        gtk_text_buffer_apply_tag(app->cbuf, app->tag_fg[app->sgr_fg - 30],
                                  &s2, &e2);
    }
}

/* GtkTextView is not a terminal: the board echoes "\b \b" to erase, so treat
 * BS as delete-previous-char, and drop lone CR (the following LF breaks). */
static void console_insert(App *app, const char *t, int len)
{
    gboolean ctl = FALSE;
    for (int i = 0; i < len; i++) {
        if (t[i] == '\b' || t[i] == '\r') {
            ctl = TRUE;
            break;
        }
    }
    if (!ctl) {
        console_raw_insert(app, t, len);
        return;
    }
    GString *run = g_string_new(NULL);
    for (int i = 0; i < len; i++) {
        char ch = t[i];
        if (ch == '\b') {
            if (run->len) {
                console_raw_insert(app, run->str, (int)run->len);
                g_string_set_size(run, 0);
            }
            GtkTextIter end, start;
            gtk_text_buffer_get_end_iter(app->cbuf, &end);
            start = end;
            if (gtk_text_iter_backward_char(&start)) {
                gtk_text_buffer_delete(app->cbuf, &start, &end);
            }
        } else if (ch == '\r') {
            continue;
        } else {
            g_string_append_c(run, ch);
        }
    }
    if (run->len) {
        console_raw_insert(app, run->str, (int)run->len);
    }
    g_string_free(run, TRUE);
}

static void sgr_apply(App *app, const char *params)
{
    if (params[0] == '\0') {            /* bare ESC[m == reset */
        app->sgr_bold = app->sgr_dim = FALSE;
        app->sgr_fg = 0;
        return;
    }
    char buf[64];
    strncpy(buf, params, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(buf, ";", &save); tok;
         tok = strtok_r(NULL, ";", &save)) {
        int c = atoi(tok);
        if (c == 0) {
            app->sgr_bold = app->sgr_dim = FALSE;
            app->sgr_fg = 0;
        } else if (c == 1) {
            app->sgr_bold = TRUE;
        } else if (c == 2) {
            app->sgr_dim = TRUE;
            app->sgr_fg = 0;
        } else if (c >= 30 && c <= 37) {
            app->sgr_fg = c;
        } else if (c >= 90 && c <= 97) {
            app->sgr_fg = c - 60;
        }
    }
}

/* End index (exclusive) of a complete CSI escape starting at s[from], or -1. */
static int csi_end(const char *s, int from, int n)
{
    if (from + 1 >= n || s[from + 1] != '[') {
        return -1;
    }
    int j = from + 2;
    while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) {
        j++;
    }
    if (j < n && ((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z'))) {
        return j + 1;
    }
    return -1;
}

/* Block cursor: a reverse-video space at the buffer tail (where the board echoes
 * typed chars).  console_append() strips it before inserting and re-adds it
 * after, so real text + backspace are untouched; cursor_blink_cb toggles the
 * tag ~twice a second.  Shown only while connected (ser_fd >= 0). */
static void cursor_off(App *app)
{
    if (!app->cur_present) {
        return;
    }
    GtkTextIter end, start;
    gtk_text_buffer_get_end_iter(app->cbuf, &end);
    start = end;
    if (gtk_text_iter_backward_char(&start)) {
        gtk_text_buffer_delete(app->cbuf, &start, &end);
    }
    app->cur_present = FALSE;
}

static void cursor_on(App *app)
{
    if (app->cur_present || app->ser_fd < 0) {
        return;
    }
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app->cbuf, &end);
    gtk_text_buffer_insert_with_tags(app->cbuf, &end, " ", 1,
                                     app->tag_cursor, NULL);
    app->cur_present = TRUE;
    app->blink_state = TRUE;             /* solid right after fresh output */
}

static gboolean cursor_blink_cb(gpointer ud)
{
    App *app = ud;
    if (app->ser_fd < 0) {               /* not connected -> no cursor */
        cursor_off(app);
        return G_SOURCE_CONTINUE;
    }
    if (!app->cur_present) {             /* connected but idle -> make one */
        cursor_on(app);
        return G_SOURCE_CONTINUE;
    }
    GtkTextIter end, start;
    gtk_text_buffer_get_end_iter(app->cbuf, &end);
    start = end;
    if (!gtk_text_iter_backward_char(&start)) {
        return G_SOURCE_CONTINUE;
    }
    app->blink_state = !app->blink_state;
    if (app->blink_state) {
        gtk_text_buffer_apply_tag(app->cbuf, app->tag_cursor, &start, &end);
    } else {
        gtk_text_buffer_remove_tag(app->cbuf, app->tag_cursor, &start, &end);
    }
    return G_SOURCE_CONTINUE;
}

void console_append(App *app, const char *data, int len)
{
    if (len < 0) {
        len = (int)strlen(data);
    }
    led_scan(app, data, len);
    cursor_off(app);                /* act on the real text end (re-added below) */

    GString *s = g_string_new_len(app->ansi_pending->str,
                                  (gssize)app->ansi_pending->len);
    g_string_append_len(s, data, len);
    g_string_set_size(app->ansi_pending, 0);
    int n = (int)s->len;

    /* Stash a trailing, incomplete escape for the next chunk. */
    int last = -1;
    for (int k = 0; k < n; k++) {
        if ((unsigned char)s->str[k] == 0x1b) {
            last = k;
        }
    }
    if (last >= 0 && (n - last) < 24 && csi_end(s->str, last, n) < 0) {
        g_string_append_len(app->ansi_pending, s->str + last, n - last);
        n = last;
    }

    int pos = 0, i = 0;
    while (i < n) {
        if ((unsigned char)s->str[i] == 0x1b) {
            int e = csi_end(s->str, i, n);
            if (e > 0) {
                if (i > pos) {
                    console_insert(app, s->str + pos, i - pos);
                }
                if (s->str[e - 1] == 'm') {     /* SGR; drop other CSI codes */
                    char params[64];
                    int pl = e - 1 - (i + 2);
                    if (pl < 0) pl = 0;
                    if (pl > 63) pl = 63;
                    memcpy(params, s->str + i + 2, (size_t)pl);
                    params[pl] = '\0';
                    sgr_apply(app, params);
                }
                pos = e;
                i = e;
                continue;
            }
        }
        i++;
    }
    if (pos < n) {
        console_insert(app, s->str + pos, n - pos);
    }
    cursor_on(app);                 /* block cursor at the new end */
    g_string_free(s, TRUE);
}

/* Bytes at the tail of s[0..n) that begin an *incomplete* UTF-8 sequence (so
 * they must be held back); 0 if s ends on a clean char boundary. GtkTextBuffer
 * rejects invalid UTF-8, so a multibyte char split across a read() boundary
 * would be mangled -- this lets console_append_serial carry the tail over. */
static int utf8_tail_hold(const char *s, int n)
{
    int cont = 0, i = n;
    while (i > 0 && cont < 3) {
        unsigned char c = (unsigned char)s[i - 1];
        if ((c & 0xC0) == 0x80) { cont++; i--; continue; }   /* continuation */
        int need;
        if      (c < 0x80)            need = 1;               /* ASCII */
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else                         return 0;                /* invalid lead */
        int have = cont + 1;
        return (have < need) ? have : 0;                      /* hold if short */
    }
    return 0;
}

/* Console path for the *serial* stream only (not app-injected messages): an
 * incremental UTF-8 decoder, the C analogue of tcon's per-connection
 * codecs.getincrementaldecoder("utf-8"). Holds back a trailing partial char. */
static void console_append_serial(App *app, const char *data, int len)
{
    if (len <= 0 && app->utf8_carry_n == 0) {
        return;
    }
    GString *u = g_string_new_len(app->utf8_carry, app->utf8_carry_n);
    if (len > 0) {
        g_string_append_len(u, data, len);
    }
    app->utf8_carry_n = 0;
    int hold = utf8_tail_hold(u->str, (int)u->len);
    if (hold > (int)u->len) {
        hold = (int)u->len;
    }
    if (hold > 0) {
        memcpy(app->utf8_carry, u->str + (int)u->len - hold, (size_t)hold);
        app->utf8_carry_n = hold;
        g_string_set_size(u, (gssize)((int)u->len - hold));
    }
    if (u->len) {
        console_append(app, u->str, (int)u->len);
    }
    g_string_free(u, TRUE);
}

static void on_scroll_value(GtkAdjustment *adj, gpointer user)
{
    App *app = user;
    app->follow = (gtk_adjustment_get_value(adj)
                   + gtk_adjustment_get_page_size(adj)
                   >= gtk_adjustment_get_upper(adj) - 24.0);
}

static void on_scroll_changed(GtkAdjustment *adj, gpointer user)
{
    App *app = user;
    if (app->follow) {
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj)
                                      - gtk_adjustment_get_page_size(adj));
    }
}

/* ------------------------------------------------------------------------- */
/* Keyboard: forward keystrokes to the board (focus-independent)             */
/* ------------------------------------------------------------------------- */

static void on_paste(GObject *src, GAsyncResult *res, gpointer user)
{
    App *app = user;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (text && app->ser_fd >= 0) {
        for (char *p = text; *p; p++) {
            if (*p == '\n') {
                *p = '\r';
            }
        }
        ser_write(app, text, strlen(text));
    }
    g_free(text);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user)
{
    (void)c;
    (void)keycode;
    App *app = user;
    if (app->ser_fd < 0) {
        return FALSE;
    }
    GtkWidget *focus = gtk_window_get_focus(app->win);
    if (focus && GTK_IS_EDITABLE(focus)) {
        return FALSE;                   /* real text fields keep their keys */
    }

    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    /* macOS Command key -> Meta (some GDK backends Super). Accept it for
     * copy/paste so Cmd-C / Cmd-V work like every other Mac app; the generic
     * Ctrl-<letter> control codes further down stay Ctrl-only. */
    gboolean cmd = (state & (GDK_META_MASK | GDK_SUPER_MASK)) != 0;
    if ((ctrl || cmd) && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
        if (gtk_text_buffer_get_has_selection(app->cbuf)) {
            return FALSE;               /* copy the selection */
        }
        if (ctrl) {                     /* bare Ctrl-C with no selection = ^C */
            ser_write(app, "\x03", 1);
            return TRUE;
        }
        return TRUE;                    /* Cmd-C, no selection: swallow */
    }
    if ((ctrl || cmd) && (keyval == GDK_KEY_v || keyval == GDK_KEY_V)) {
        GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(app->cview));
        gdk_clipboard_read_text_async(cb, NULL, on_paste, app);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        ser_write(app, "\r", 1);
        return TRUE;
    }
    if (keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete) {
        ser_write(app, "\x08", 1);
        return TRUE;
    }
    if (keyval == GDK_KEY_Tab) {
        ser_write(app, "\t", 1);
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape) {
        ser_write(app, "\x1b", 1);
        return TRUE;
    }
    if (ctrl) {                         /* other Ctrl-<letter> */
        guint lk = gdk_keyval_to_lower(keyval);
        if (lk >= GDK_KEY_a && lk <= GDK_KEY_z) {
            char ch = (char)(lk - GDK_KEY_a + 1);
            ser_write(app, &ch, 1);
            return TRUE;
        }
    }
    gunichar uch = gdk_keyval_to_unicode(keyval);
    if (uch >= 0x20 && uch != 0x7f) {
        char u[6];
        int l = g_unichar_to_utf8(uch, u);
        ser_write(app, u, (size_t)l);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Serial I/O + SLIP demux                                                   */
/* ------------------------------------------------------------------------- */

static gboolean on_serial_io(gint fd, GIOCondition cond, gpointer user)
{
    (void)cond;
    App *app = user;
    uint8_t buf[4096];
    ssize_t nr = read(fd, buf, sizeof(buf));
    if (nr <= 0) {
        if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return G_SOURCE_CONTINUE;
        }
        teardown(app);                  /* serial vanished (unplugged) */
        return G_SOURCE_REMOVE;
    }

    GString *text = g_string_new(NULL);
    for (ssize_t i = 0; i < nr; i++) {
        uint8_t b = buf[i];
        if (b == SLIP_END) {
            /* END flushes pending console text, ends any frame, and re-arms: the
             * next byte starts a frame only if it is an IPv4 nibble (0x4N), and
             * an emitted frame is validated (version + total length).  Anchoring
             * on END+0x4N rather than a bare END toggle makes the demux
             * self-syncing, so a dropped byte (J-Link VCOM under back-to-back
             * load) costs at most the one damaged frame instead of permanently
             * flipping the toggle phase and cascade-dropping all frames after. */
            if (text->len) {
                console_append_serial(app, text->str, (int)text->len);
                wifi_feed(app, text->str, (int)text->len);
                g_string_set_size(text, 0);
            }
            if (app->in_frame && app->frame->len) {
                uint8_t ip[BRIDGE_MTU * 2];
                size_t iplen = slip_unescape(app->frame->data,
                                             app->frame->len, ip);
                if (iplen >= 20 && (ip[0] & 0xF0) == 0x40 &&
                    (((size_t)ip[2] << 8) | ip[3]) == iplen) {
                    net_on_ip_packet(app, ip, iplen);
                }
                /* else: mis-framed (a byte was lost) -> drop and re-sync */
            }
            g_byte_array_set_size(app->frame, 0);
            app->in_frame = FALSE;
            app->slip_armed = TRUE;
        } else if (app->slip_armed && (b & 0xF0) == 0x40) {
            app->slip_armed = FALSE;
            app->in_frame = TRUE;
            g_byte_array_set_size(app->frame, 0);
            g_byte_array_append(app->frame, &b, 1);
        } else if (app->in_frame) {
            /* cap the frame at the decode buffer so a garbled/hostile stream
             * can't grow it unbounded or overflow ip[BRIDGE_MTU*2] above */
            if (app->frame->len < (guint)(BRIDGE_MTU * 2)) {
                g_byte_array_append(app->frame, &b, 1);
            }
        } else {
            app->slip_armed = FALSE;
            g_string_append_c(text, (char)b);
        }
    }
    if (text->len) {
        console_append_serial(app, text->str, (int)text->len);
        wifi_feed(app, text->str, (int)text->len);
    }
    g_string_free(text, TRUE);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------- */
/* Connect / disconnect                                                      */
/* ------------------------------------------------------------------------- */

static void do_connect(App *app)
{
    if (app->ser_fd >= 0) {
        teardown(app);
        return;
    }
    if (!app->port_path[0]) {
        set_status(app, "no serial port -- plug in a board and rescan", TRUE);
        return;
    }
    unsigned baud = (unsigned)atoi(baud_get_text(app));
    int fd = serial_open(app->port_path, baud);
    if (fd < 0) {
        char e[400];
        snprintf(e, sizeof(e), "error opening %s: %s", app->port_path,
                 strerror(errno));
        set_status(app, e, TRUE);
        return;
    }
    app->ser_fd = fd;
    app->utf8_carry_n = 0;          /* fresh UTF-8 decoder per connection */
    app->ser_watch = g_unix_fd_add(fd, G_IO_IN, on_serial_io, app);
    gtk_button_set_label(GTK_BUTTON(app->connect_btn), "Disconnect");
    gtk_widget_remove_css_class(GTK_WIDGET(app->cview), "console-off");
    update_leds(app);

    char st[420];
    snprintf(st, sizeof(st), "connected to %s @ %u baud", app->port_path, baud);
    set_status(app, st, FALSE);
    static const char *hello =
        "[tikuconsole] connected (console mode) -- type away.\n";
    console_append(app, hello, (int)strlen(hello));
    gtk_widget_grab_focus(GTK_WIDGET(app->cview));

    /* show the side panel so the Wi-Fi controls are reachable, and run a quiet
     * status sync in case the board auto-rejoined Wi-Fi on a cold boot. */
    gtk_widget_set_visible(app->netpanel, TRUE);
    g_timeout_add(1500, wifi_sync_cb, app);

    if (gtk_switch_get_active(GTK_SWITCH(app->net_sw))) {  /* net pre-selected */
        net_apply(app, TRUE);
    }
}

/* Connect over BLE (CoreBluetooth) instead of a serial port: the Nordic UART
 * Service is bridged to a socket fd (ble_mac.m) that we drive through the exact
 * same console path as a serial link. */
static void do_connect_ble(App *app)
{
    if (app->ser_fd >= 0) {                 /* already connected -> disconnect */
        teardown(app);
        return;
    }
    int fd = ble_mac_open("tikuOS");
    if (fd < 0) {
        set_status(app, "BLE: could not start CoreBluetooth "
                        "(is Bluetooth on / permitted?)", TRUE);
        return;
    }
    app->ser_fd = fd;
    app->ble_on = TRUE;
    app->utf8_carry_n = 0;
    app->ser_watch = g_unix_fd_add(fd, G_IO_IN, on_serial_io, app);
    gtk_button_set_label(GTK_BUTTON(app->connect_btn), "Disconnect");
    gtk_button_set_label(GTK_BUTTON(app->ble_btn), "BLE \xe2\x9c\x95");  /* ✕ */
    gtk_widget_remove_css_class(GTK_WIDGET(app->cview), "console-off");
    update_leds(app);
    set_status(app, "BLE: scanning for \"tikuOS\" over the Nordic UART Service…",
               FALSE);
    static const char *hello =
        "[tikuconsole] BLE -- connecting to \"tikuOS\" over the "
        "Nordic UART Service…\n";
    console_append(app, hello, (int)strlen(hello));
    gtk_widget_grab_focus(GTK_WIDGET(app->cview));
}

static void teardown(App *app)
{
    if (app->ser_watch) {
        g_source_remove(app->ser_watch);
        app->ser_watch = 0;
    }
    net_down(app);
    ping_cancel(app);
    if (app->ble_on) {                      /* tear down the CoreBluetooth link */
        ble_mac_close();
        app->ble_on = FALSE;
        if (app->ble_btn) {
            gtk_button_set_label(GTK_BUTTON(app->ble_btn), "BLE");
        }
    }
    if (app->ser_fd >= 0) {
        close(app->ser_fd);
        app->ser_fd = -1;
    }
    app->slip_on = FALSE;
    app->in_frame = FALSE;
    app->slip_armed = FALSE;
    g_byte_array_set_size(app->frame, 0);
    /* Wi-Fi: drop the link state + hide the side panel on disconnect */
    app->wifi_joined = FALSE;
    app->wifi_capture = WIFI_CAP_NONE;
    app->wifi_ip_shown[0] = '\0';
    gtk_widget_set_visible(app->netpanel, FALSE);
    gtk_button_set_label(GTK_BUTTON(app->connect_btn), "Connect");
    gtk_widget_add_css_class(GTK_WIDGET(app->cview), "console-off");
    update_leds(app);
    set_status(app, "disconnected", FALSE);
}

/* ------------------------------------------------------------------------- */
/* File-op console guard                                                     */
/*                                                                           */
/* A /data transfer (gui_files.c) drives a synchronous request/response on   */
/* the serial fd, so it must own the fd for its duration.  Pause the async   */
/* console read-watch around the call and restore it after -- any bytes that */
/* arrive meanwhile stay in the kernel buffer and drain when it resumes.     */
/* ------------------------------------------------------------------------- */

int files_pause_console(App *app)
{
    if (app->ser_fd < 0) {
        return -1;
    }
    if (app->ser_watch) {
        g_source_remove(app->ser_watch);
        app->ser_watch = 0;
    }
    return app->ser_fd;
}

void files_resume_console(App *app)
{
    if (app->ser_fd >= 0 && app->ser_watch == 0) {
        app->ser_watch = g_unix_fd_add(app->ser_fd, G_IO_IN, on_serial_io, app);
    }
}

/* ------------------------------------------------------------------------- */
/* Ports                                                                     */
/* ------------------------------------------------------------------------- */

static void port_changed_core(App *app)
{
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->port_dd));
    if (i < (guint)app->n_ports) {
        port_info_t *p = &app->ports[i];
        strlcpy(app->port_path, p->device, sizeof(app->port_path));
        gtk_label_set_text(GTK_LABEL(app->platform_lbl), p->label);
        set_wifi_pane_visible(app, p->label);   /* WiFi pane only on RP2350-class */
        if (app->ser_fd < 0) {          /* don't fight a live session */
            char b[16];
            snprintf(b, sizeof(b), "%u", p->baud);
            baud_set_text(app, b);
        }
    } else {
        app->port_path[0] = '\0';
        gtk_label_set_text(GTK_LABEL(app->platform_lbl), "--");
        set_wifi_pane_visible(app, "");
    }
}

/* A J-Link exposes debugger and target-console VCOMs under the same USB
 * serial.  The second callout is the target UART on Apollo and nRF54L15. */
static int console_vcom_index(App *app, int candidate)
{
    if (candidate < 0 || candidate >= app->n_ports ||
        app->ports[candidate].vid != 0x1366) {
        return candidate;
    }
    int found = 0;
    for (int i = 0; i < app->n_ports; i++) {
        gboolean same = app->ports[i].vid == 0x1366;
        if (app->ports[candidate].serial[0]) {
            same = same && strcmp(app->ports[i].serial,
                                  app->ports[candidate].serial) == 0;
        } else {
            same = same && app->ports[i].pid == app->ports[candidate].pid;
        }
        if (same && ++found == 2) {
            return i;
        }
    }
    return candidate;
}

static void on_port_changed(GObject *o, GParamSpec *ps, gpointer user)
{
    (void)o;
    (void)ps;
    port_changed_core((App *)user);
}

static void refresh_ports(App *app)
{
    char previous[sizeof(app->port_path)];
    strlcpy(previous, app->port_path, sizeof(previous));
    app->n_ports = ports_scan(app->ports, PORTS_MAX);
    GtkStringList *sl = gtk_string_list_new(NULL);
    if (app->n_ports == 0) {
        gtk_string_list_append(sl, "(no USB serial ports)");
    } else {
        for (int i = 0; i < app->n_ports; i++) {
            const char *dev = app->ports[i].device;
            const char *base = strrchr(dev, '/');
            base = base ? base + 1 : dev;
            char lbl[340];
            snprintf(lbl, sizeof(lbl), "%s  \xc2\xb7  %s", base,
                     app->ports[i].label);
            gtk_string_list_append(sl, lbl);
        }
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(app->port_dd), G_LIST_MODEL(sl));
    g_object_unref(sl);

    int selected = -1;
    if (previous[0]) {
        for (int i = 0; i < app->n_ports; i++) {
            if (strcmp(app->ports[i].device, previous) == 0) {
                selected = i;                    /* never yank a live choice */
                break;
            }
        }
    }
    if (selected < 0) {
        for (int i = 0; i < app->n_ports; i++) {
            if (strcmp(app->ports[i].label, "unknown") != 0 &&
                strstr(app->ports[i].label, "eZ-FET") == NULL) {
                selected = console_vcom_index(app, i);
                break;
            }
        }
    }
    if (selected < 0 && app->n_ports > 0) {
        selected = 0;
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->port_dd),
                               selected >= 0 ? (guint)selected : 0);
    port_changed_core(app);
    bld_autoselect(app);            /* re-detect the board's MCU family too */
}

static void on_refresh_clicked(GtkButton *b, gpointer user)
{
    (void)b;
    refresh_ports((App *)user);
}

static void on_connect_clicked(GtkButton *b, gpointer user)
{
    (void)b;
    do_connect((App *)user);
}

static void on_ble_clicked(GtkButton *b, gpointer user)
{
    (void)b;
    do_connect_ble((App *)user);
}

static void on_files_clicked(GtkButton *b, gpointer user)
{
    (void)b;
    files_window_open((App *)user);
}

static gboolean on_net_toggle(GtkSwitch *sw, gboolean state, gpointer user)
{
    (void)sw;
    App *app = user;
    if (app->ser_fd < 0) {              /* not connected: remember the choice */
        gtk_widget_set_visible(app->netpanel, state);
        if (state && geteuid() != 0) {
            set_status(app, "Networking mode -- the host utun/NAT bridge needs "
                            "sudo (SLIP + board ping work without it)", FALSE);
        }
        return FALSE;
    }
    net_apply(app, state);
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Hooks for the build bar (gui_build.c)                                     */
/* ------------------------------------------------------------------------- */

void gui_disconnect(App *app)
{
    if (app->ser_fd >= 0) {
        teardown(app);                  /* flashing reuses the same debugger */
    }
}

gboolean gui_autoconnect_step(App *app)
{
    if (app->ser_fd >= 0) {
        return TRUE;
    }
    refresh_ports(app);
    if (app->port_path[0]) {
        do_connect(app);
        if (app->ser_fd >= 0) {
            gtk_widget_grab_focus(GTK_WIDGET(app->cview));
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Window assembly                                                           */
/* ------------------------------------------------------------------------- */

/* Register the bundled logo dir as an icon search path + name the window icon,
 * mirroring tcon/app.py. macOS shows no titlebar icon for a non-bundled GTK
 * app, so this is largely a no-op here, but it is correct and works if bundled. */
static void install_window_icon(GtkWindow *win)
{
    char exe[2048];
    uint32_t sz = sizeof(exe);
    if (_NSGetExecutablePath(exe, &sz) != 0) {
        return;
    }
    char *real = realpath(exe, NULL);
    if (!real) {
        return;
    }
    char *dir  = g_path_get_dirname(real);                  /* <tikuConsole>/mac */
    char *logo = g_build_filename(dir, "..", "logo", NULL); /* <tikuConsole>/logo */
    GtkIconTheme *theme =
        gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_icon_theme_add_search_path(theme, logo);
    gtk_window_set_icon_name(win, "org.tikuos.tikuconsole");
    g_free(logo);
    g_free(dir);
    free(real);
}

static void install_css(void)
{
    static const char *data =
        "textview.console, textview.console text {"
        " background-color:#0b0b0b; color:#cccccc;"
        " font-family:\"Menlo\",\"SF Mono\",\"Monaco\",monospace;"
        " font-size:11pt; }"
        "textview.console { padding:4px; }"
        "textview.console.console-off { opacity:0.45; }"
        ".slip-led { color:#555555;"
        " transition:color 500ms ease-out, text-shadow 500ms ease-out; }"
        ".slip-led.tx-on { color:#36c5f0; text-shadow:0 0 8px #36c5f0; }"
        ".slip-led.rx-on { color:#5fd35f; text-shadow:0 0 8px #5fd35f; }"
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
        " linear-gradient(90deg,#1f6fc4,#2e9e54,#f0a91e); }";
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, data);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void make_console_tags(App *app)
{
    static const struct { int code; const char *color; } PAL[] = {
        {30, "#666666"}, {31, "#ff6b6b"}, {32, GREEN},    {33, "#fce94f"},
        {34, "#729fcf"}, {35, "#ad7fa8"}, {36, "#34e2e2"}, {37, "#d3d7cf"},
    };
    for (int i = 0; i < 8; i++) {
        app->tag_fg[i] = gtk_text_buffer_create_tag(app->cbuf, NULL,
            "foreground", PAL[i].color, NULL);
    }
    app->tag_bold = gtk_text_buffer_create_tag(app->cbuf, NULL,
        "weight", PANGO_WEIGHT_BOLD, NULL);
    app->tag_dim = gtk_text_buffer_create_tag(app->cbuf, NULL,
        "foreground", "#7f7f7f", NULL);
    app->tag_cursor = gtk_text_buffer_create_tag(app->cbuf, NULL,
        "background", "#cccccc", NULL);
    g_timeout_add(530, cursor_blink_cb, app);   /* blink the block cursor */
}

/* Copy the entire console + build/flash log to the system clipboard. */
static void on_copy_log_clicked(GtkButton *b, gpointer user)
{
    App *app = (App *)user;
    GtkTextIter s, e;
    char *text;

    gtk_text_buffer_get_bounds(app->cbuf, &s, &e);
    text = gtk_text_buffer_get_text(app->cbuf, &s, &e, FALSE);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(b)), text);
    g_free(text);
}

static gboolean smoke_quit(gpointer user)
{
    g_application_quit(G_APPLICATION(((App *)user)->app));
    return G_SOURCE_REMOVE;
}

static void activate(GtkApplication *gapp, gpointer user)
{
    App *app = user;
    install_css();

    GtkWidget *win = gtk_application_window_new(gapp);
    app->win = GTK_WINDOW(win);
    install_window_icon(app->win);
    gtk_window_set_title(app->win, "TikuConsole");
    gtk_window_set_default_size(app->win, 960, 600);
    gtk_window_set_titlebar(app->win, gtk_header_bar_new());

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 8);
    gtk_widget_set_margin_start(root, 8);
    gtk_widget_set_margin_end(root, 8);
    gtk_window_set_child(app->win, root);

    /* --- banner row: title (left) + status lights (right) --- */
    GtkWidget *brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *banner = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(banner), 0);
    gtk_widget_set_hexpand(banner, TRUE);
    char bmk[600];
    snprintf(bmk, sizeof(bmk),
        "<span size='xx-large' weight='bold' foreground='%s'>TikuConsole</span>"
        "  <span size='small' foreground='#888888'>v%s</span>\n"
        "<span size='small' foreground='#888888'>serial console for TikuOS "
        "devices  \xc2\xb7  networking optional</span>", GREEN, VERSION);
    gtk_label_set_markup(GTK_LABEL(banner), bmk);
    gtk_box_append(GTK_BOX(brow), banner);

    GtkWidget *leds = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_valign(leds, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(leds, GTK_ALIGN_END);
    app->usb_led = gtk_label_new(NULL);
    app->wifi_led = gtk_label_new(NULL);
    app->wifi_ip_chip = gtk_label_new(NULL);
    gtk_label_set_selectable(GTK_LABEL(app->wifi_ip_chip), TRUE);
    app->slip_led = gtk_label_new(NULL);
    app->nat_led = gtk_label_new(NULL);
    gtk_box_append(GTK_BOX(leds), app->usb_led);
    gtk_box_append(GTK_BOX(leds), app->wifi_led);
    gtk_box_append(GTK_BOX(leds), app->wifi_ip_chip);
    gtk_box_append(GTK_BOX(leds), app->slip_led);
    gtk_box_append(GTK_BOX(leds), app->nat_led);
    gtk_box_append(GTK_BOX(brow), leds);
    gtk_box_append(GTK_BOX(root), brow);

    /* --- firmware build/flash bar (compile + program from here) --- */
    gtk_box_append(GTK_BOX(root), build_buildbar(app));

    /* --- connection bar --- */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Port"));
    app->port_dd = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(NULL)),
                                     NULL);
    g_signal_connect(app->port_dd, "notify::selected",
                     G_CALLBACK(on_port_changed), app);
    gtk_box_append(GTK_BOX(bar), app->port_dd);
    GtkWidget *refresh = gtk_button_new_with_label("\xe2\x9f\xb3");
    gtk_widget_set_tooltip_text(refresh, "Rescan ports");
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_clicked), app);
    gtk_box_append(GTK_BOX(bar), refresh);
    app->platform_lbl = gtk_label_new("--");
    gtk_widget_add_css_class(app->platform_lbl, "dim-label");
    gtk_box_append(GTK_BOX(bar), app->platform_lbl);
    gtk_box_append(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Baud"));
    app->baud = gtk_drop_down_new_from_strings(BAUD_RATES);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->baud), BAUD_DEFAULT);
    gtk_box_append(GTK_BOX(bar), app->baud);
    gtk_box_append(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Networking"));
    app->net_sw = gtk_switch_new();
    gtk_widget_set_valign(app->net_sw, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(app->net_sw,
        "Bring up SLIP/IP + utun over the same wire (host bridge needs sudo)");
    g_signal_connect(app->net_sw, "state-set", G_CALLBACK(on_net_toggle), app);
    gtk_box_append(GTK_BOX(bar), app->net_sw);
    gtk_box_append(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    app->files_btn = gtk_button_new_with_label("Files\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(app->files_btn,
        "Browse and transfer files in the device's /data store");
    g_signal_connect(app->files_btn, "clicked", G_CALLBACK(on_files_clicked), app);
    gtk_box_append(GTK_BOX(bar), app->files_btn);
    app->ble_btn = gtk_button_new_with_label("BLE");
    gtk_widget_set_tooltip_text(app->ble_btn,
        "Connect to a board's wireless shell over Bluetooth LE "
        "(Nordic UART Service); run `ble uart` on the board first");
    g_signal_connect(app->ble_btn, "clicked", G_CALLBACK(on_ble_clicked), app);
    gtk_box_append(GTK_BOX(bar), app->ble_btn);
    gtk_widget_set_visible(app->ble_btn, FALSE);  /* Blue board only (build bar) */
    GtkWidget *copy_btn = gtk_button_new_with_label("Copy log");
    gtk_widget_set_tooltip_text(copy_btn,
        "Copy the whole console + build/flash log to the clipboard");
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_log_clicked), app);
    gtk_box_append(GTK_BOX(bar), copy_btn);
    app->connect_btn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(app->connect_btn, "suggested-action");
    gtk_widget_set_hexpand(app->connect_btn, TRUE);
    gtk_widget_set_halign(app->connect_btn, GTK_ALIGN_END);
    g_signal_connect(app->connect_btn, "clicked",
                     G_CALLBACK(on_connect_clicked), app);
    gtk_box_append(GTK_BOX(bar), app->connect_btn);
    gtk_box_append(GTK_BOX(root), bar);

    /* --- status line --- */
    app->status = gtk_label_new("disconnected");
    gtk_label_set_xalign(GTK_LABEL(app->status), 0);
    gtk_label_set_selectable(GTK_LABEL(app->status), TRUE);
    gtk_label_set_wrap(GTK_LABEL(app->status), TRUE);
    gtk_box_append(GTK_BOX(root), app->status);

    /* --- main row: console | (optional) network panel, draggable divider --- */
    app->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(app->paned, TRUE);
    gtk_paned_set_wide_handle(GTK_PANED(app->paned), TRUE);
    gtk_box_append(GTK_BOX(root), app->paned);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(sw, TRUE);
    app->cview = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(app->cview, FALSE);
    gtk_text_view_set_monospace(app->cview, TRUE);
    gtk_text_view_set_wrap_mode(app->cview, GTK_WRAP_CHAR);
    app->cbuf = gtk_text_view_get_buffer(app->cview);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw),
                                  GTK_WIDGET(app->cview));
    app->cadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    gtk_widget_add_css_class(GTK_WIDGET(app->cview), "console");
    gtk_widget_add_css_class(GTK_WIDGET(app->cview), "console-off");
    make_console_tags(app);
    g_signal_connect(app->cadj, "value-changed",
                     G_CALLBACK(on_scroll_value), app);
    g_signal_connect(app->cadj, "changed",
                     G_CALLBACK(on_scroll_changed), app);
    gtk_paned_set_start_child(GTK_PANED(app->paned), sw);
    gtk_paned_set_resize_start_child(GTK_PANED(app->paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(app->paned), FALSE);

    app->netpanel = build_netpanel(app);
    gtk_widget_set_visible(app->netpanel, FALSE);   /* shown only in net mode */
    gtk_paned_set_end_child(GTK_PANED(app->paned), app->netpanel);
    gtk_paned_set_resize_end_child(GTK_PANED(app->paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(app->paned), FALSE);
    gtk_paned_set_position(GTK_PANED(app->paned), 600);

    /* Type straight into the console from anywhere in the window. */
    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), app);
    gtk_widget_add_controller(win, kc);

    g_timeout_add(500, net_counters_tick, app);
    refresh_ports(app);
    update_leds(app);
    bld_update_ble_ui(app);         /* board-specific firmware controls */

    const char *smoke = g_getenv("TIKUCONSOLE_SMOKE_MS");
    gboolean force_splash = g_getenv("TIKUCONSOLE_FORCE_SPLASH") != NULL;
    gboolean direct = (smoke != NULL ||
                       g_getenv("TIKUCONSOLE_NO_SPLASH") != NULL) && !force_splash;
    if (direct) {
        gtk_window_present(app->win);
        gtk_widget_grab_focus(GTK_WIDGET(app->cview));
    } else {
        show_splash(app);                  /* presents the main window when done */
    }

    if (smoke) {
        gtk_widget_set_visible(app->netpanel, TRUE);  /* exercise the panel +
                                                          its drawing areas */
        bld_debug_dump(app);                          /* proj_dir + make flags */
        int ms = atoi(smoke);
        g_timeout_add(ms > 0 ? (guint)ms : 1, smoke_quit, app);
    }
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (g_getenv("TIKUCONSOLE_SCAN")) {     /* headless port dump, no GTK/display */
        port_info_t p[PORTS_MAX];
        int n = ports_scan(p, PORTS_MAX);
        if (n == 0) {
            printf("no USB serial ports found\n");
        }
        for (int i = 0; i < n; i++) {
            printf("%-18s %04x:%04x  %-24s baud=%u\n", p[i].device,
                   p[i].vid < 0 ? 0u : (unsigned)p[i].vid,
                   p[i].pid < 0 ? 0u : (unsigned)p[i].pid,
                   p[i].label, p[i].baud);
        }
        return 0;
    }
    App *app = g_new0(App, 1);
    app->ser_fd = -1;
    app->utun_fd = -1;
    app->follow = TRUE;
    app->ping_ident = 0x4242;
    app->frame = g_byte_array_new();
    app->ansi_pending = g_string_new(NULL);
    app->slip_scan = g_string_new(NULL);
    app->wifi_linebuf = g_string_new(NULL);
    app->wifi_aps = g_ptr_array_new_with_free_func(g_free);
    app->wifi_auto_up = TRUE;

    app->app = gtk_application_new("org.tikuos.tikuconsole",
                                   G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app->app, "activate", G_CALLBACK(activate), app);
    int status = g_application_run(G_APPLICATION(app->app), argc, argv);

    g_object_unref(app->app);
    g_byte_array_free(app->frame, TRUE);
    g_string_free(app->ansi_pending, TRUE);
    g_string_free(app->slip_scan, TRUE);
    g_string_free(app->wifi_linebuf, TRUE);
    g_ptr_array_free(app->wifi_aps, TRUE);
    g_free(app);
    return status;
}
