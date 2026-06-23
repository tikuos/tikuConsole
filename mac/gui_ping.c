/*
 * gui_ping.c - TikuConsole in-app ICMP-over-SLIP pinger + its charts.
 *
 * Ports tcon/ping.py: craft ICMP echo requests in userspace, SLIP them to the
 * board (whose own stack answers), match the replies as they come back through
 * the demux, and draw the ping(8)-style stats, the RTT sparkline, and the
 * animated this-Mac -> board illustration.  No TUN, no system `ping`, no root.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>

#include <gtk/gtk.h>

#include "bridge.h"
#include "gui.h"
#include "packets.h"

static gboolean ping_tick(gpointer user);
static gboolean ping_finish(gpointer user);
static gboolean ping_anim_tick(gpointer user);
static void ping_row(App *app, const char *text, GtkTextTag *tag);
static void ping_stats_update(App *app);

/* ------------------------------------------------------------------------- */
/* Pinger                                                                    */
/* ------------------------------------------------------------------------- */

void ping_start(App *app, const char *target)
{
    if (app->ping_active) {                  /* one run at a time */
        return;
    }
    if (app->ser_fd < 0) {
        gtk_label_set_text(GTK_LABEL(app->ping_stats), "connect first");
        return;
    }
    char t[64];
    strlcpy(t, target ? target : "", sizeof(t));
    char *s = t;                             /* trim surrounding spaces */
    while (*s == ' ') {
        s++;
    }
    size_t L = strlen(s);
    while (L > 0 && s[L - 1] == ' ') {
        s[--L] = '\0';
    }
    if (!*s) {
        return;
    }

    app->ping_active = TRUE;
    app->ping_n_rtt = app->ping_sent = app->ping_recv = 0;
    memset(app->ping_pending, 0, sizeof(app->ping_pending));
    strlcpy(app->ping_target, s, sizeof(app->ping_target));
    app->ping_ident = (uint16_t)((app->ping_ident + 1) & 0xffff);
    app->ping_i = 0;
    app->ping_n = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->ping_spin));
    gtk_text_buffer_set_text(app->ping_buf, "", -1);
    gtk_widget_queue_draw(app->spark);

    char m[96];
    snprintf(m, sizeof(m), "pinging %s over SLIP ...", s);
    gtk_label_set_text(GTK_LABEL(app->ping_stats), m);
    gtk_widget_set_sensitive(app->ping_btn, FALSE);
    app->ping_n_anim = 0;
    if (!app->ping_anim_src) {               /* drive the packet animation */
        app->ping_anim_src = g_timeout_add(50, ping_anim_tick, app);
    }
    send_line(app, "slip on");               /* ensure board SLIP (idempotent) */
    g_timeout_add(350, ping_tick, app);      /* settle, then one probe/tick */
}

static gboolean ping_tick(gpointer user)
{
    App *app = user;
    if (!app->ping_active) {                 /* cancelled (disconnect / off) */
        return G_SOURCE_REMOVE;
    }
    if (app->ping_i >= app->ping_n) {        /* all sent -> wait, then finish */
        g_timeout_add(1000, ping_finish, app);
        return G_SOURCE_REMOVE;
    }
    int seq = app->ping_i++;
    struct in_addr dst;
    if (inet_aton(app->ping_target, &dst) == 0) {
        char m[96];
        snprintf(m, sizeof(m), "bad address: %s", app->ping_target);
        gtk_label_set_text(GTK_LABEL(app->ping_stats), m);
        app->ping_active = FALSE;
        gtk_widget_set_sensitive(app->ping_btn, TRUE);
        return G_SOURCE_REMOVE;
    }
    uint8_t src[4] = {172, 16, 7, 1}, d[4];
    memcpy(d, &dst.s_addr, 4);

    uint8_t icmp[64], ip[128], enc[300];
    size_t il = build_icmp_echo(app->ping_ident, (uint16_t)seq,
                                (const uint8_t *)"tikuconsole", 11, icmp);
    size_t pl = build_ip(src, d, 1, icmp, il, 0, ip);
    app->ping_send_t[seq] = g_get_monotonic_time();
    app->ping_pending[seq] = TRUE;
    size_t el = slip_encode(ip, pl, enc);
    ser_write(app, (const char *)enc, el);
    app->ping_sent++;
    app->fr_out++;
    app->by_out += (unsigned)pl;
    if (app->ping_n_anim < 256) {            /* one moving icon per probe */
        app->ping_anim[app->ping_n_anim++] = 0.0;
    }
    return G_SOURCE_CONTINUE;
}

void ping_on_reply(App *app, const uint8_t *pkt, size_t len)
{
    int seq = parse_icmp_echo_reply(pkt, len, app->ping_ident);
    if (seq < 0 || seq >= PING_MAX || !app->ping_pending[seq]) {
        return;                              /* not ours / dup / finished */
    }
    app->ping_pending[seq] = FALSE;
    double rtt = (double)(g_get_monotonic_time() - app->ping_send_t[seq]) / 1000.0;
    if (app->ping_n_rtt < PING_MAX) {
        app->ping_rtts[app->ping_n_rtt++] = rtt;
    }
    app->ping_recv++;
    app->ping_pulse = 8;                      /* flash the board glyph */

    double mx = 0.0;
    for (int i = 0; i < app->ping_n_rtt; i++) {
        if (app->ping_rtts[i] > mx) {
            mx = app->ping_rtts[i];
        }
    }
    int barn = (int)lround(20.0 * rtt / (mx > 0 ? mx : 1.0));
    if (barn < 1) barn = 1;
    if (barn > 20) barn = 20;
    char bar[3 * 20 + 1];
    int bo = 0;
    for (int i = 0; i < barn; i++) {          /* U+2588 FULL BLOCK */
        bar[bo++] = (char)0xe2;
        bar[bo++] = (char)0x96;
        bar[bo++] = (char)0x88;
    }
    bar[bo] = '\0';
    char row[160];
    snprintf(row, sizeof(row), "packet %-3d %7.1f ms  %s", seq, rtt, bar);
    ping_row(app, row, app->ping_ok);
    ping_stats_update(app);
    gtk_widget_queue_draw(app->spark);
}

static gboolean ping_finish(gpointer user)
{
    App *app = user;
    if (!app->ping_active) {
        return G_SOURCE_REMOVE;
    }
    app->ping_active = FALSE;
    gtk_widget_set_sensitive(app->ping_btn, TRUE);
    for (int seq = 0; seq < app->ping_n; seq++) {   /* never answered */
        if (app->ping_pending[seq]) {
            app->ping_pending[seq] = FALSE;
            char m[96];
            snprintf(m, sizeof(m), "packet %-3d  no reply (timed out)", seq);
            ping_row(app, m, app->ping_bad);
        }
    }
    int n = app->ping_n_rtt, sent = app->ping_sent, recv = app->ping_recv;
    int loss = sent ? (int)lround(100.0 * (sent - recv) / sent) : 0;
    GtkTextTag *tag = recv ? app->ping_ok : app->ping_bad;
    char m[160];
    snprintf(m, sizeof(m), "--- %s ping statistics ---", app->ping_target);
    ping_row(app, m, tag);
    snprintf(m, sizeof(m), "%d packets sent, %d received, %d%% packet loss",
             sent, recv, loss);
    ping_row(app, m, tag);
    if (n > 0) {
        double mn = app->ping_rtts[0], mx = app->ping_rtts[0], sum = 0;
        for (int i = 0; i < n; i++) {
            double v = app->ping_rtts[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
        }
        snprintf(m, sizeof(m), "rtt  min %.1f / avg %.1f / max %.1f ms",
                 mn, sum / n, mx);
        ping_row(app, m, tag);
    }
    ping_stats_update(app);
    gtk_widget_queue_draw(app->spark);
    return G_SOURCE_REMOVE;
}

void ping_cancel(App *app)
{
    app->ping_active = FALSE;                 /* the anim timer drains itself */
    if (app->ping_btn) {
        gtk_widget_set_sensitive(app->ping_btn, TRUE);
    }
}

static void ping_row(App *app, const char *text, GtkTextTag *tag)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app->ping_buf, &end);
    char line[200];
    snprintf(line, sizeof(line), "%s\n", text);
    gtk_text_buffer_insert_with_tags(app->ping_buf, &end, line, -1, tag, NULL);
    GtkTextMark *mk = gtk_text_buffer_get_insert(app->ping_buf);
    gtk_text_view_scroll_to_mark(app->ping_view, mk, 0, FALSE, 0, 0);
}

static void ping_stats_update(App *app)
{
    int n = app->ping_n_rtt, sent = app->ping_sent;
    if (n > 0) {
        double mn = app->ping_rtts[0], mx = app->ping_rtts[0], sum = 0;
        for (int i = 0; i < n; i++) {
            double v = app->ping_rtts[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
        }
        int loss = sent ? (int)lround(100.0 * (sent - n) / sent) : 0;
        char m[220];
        snprintf(m, sizeof(m),
                 "%d sent \xc2\xb7 %d received \xc2\xb7 %d%% lost     "
                 "round-trip min %.1f / avg %.1f / max %.1f ms",
                 sent, n, loss, mn, sum / n, mx);
        gtk_label_set_text(GTK_LABEL(app->ping_stats), m);
    } else if (sent) {
        char m[160];
        snprintf(m, sizeof(m), "%d sent \xc2\xb7 0 received \xc2\xb7 100%% lost  "
                 "-- no replies (is SLIP on / NAT needed?)", sent);
        gtk_label_set_text(GTK_LABEL(app->ping_stats), m);
    } else {
        gtk_label_set_text(GTK_LABEL(app->ping_stats),
                           "idle -- enter an address and click Ping");
    }
}

static gboolean ping_anim_tick(gpointer user)
{
    App *app = user;
    int w = 0;                                /* advance + drop arrivals */
    for (int i = 0; i < app->ping_n_anim; i++) {
        double p = app->ping_anim[i] + 0.033;
        if (p < 1.0) {
            app->ping_anim[w++] = p;
        }
    }
    app->ping_n_anim = w;
    if (app->ping_pulse > 0) {
        app->ping_pulse--;
    }
    gtk_widget_queue_draw(app->ping_anim_area);
    if (!app->ping_active && app->ping_pulse <= 0 && app->ping_n_anim == 0) {
        app->ping_anim_src = 0;               /* idle -> stop animating */
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------- */
/* Cairo charts                                                              */
/* ------------------------------------------------------------------------- */

static void ctext(cairo_t *cr, double x, double y, const char *s,
                  double size, double g)
{
    cairo_set_source_rgb(cr, g, g, g);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    cairo_move_to(cr, x - e.width / 2.0, y);
    cairo_show_text(cr, s);
}

/* this Mac -> board over the wire: a monitor, the controller chip, IPs below,
 * amber packets sliding across (the chip glows green on each reply). */
void ping_draw_anim(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                    gpointer user)
{
    (void)area;
    App *app = user;
    cairo_set_source_rgb(cr, 0.043, 0.043, 0.043);
    cairo_paint(cr);
    double midy = h * 0.46;
    double pcx = 28.0, bdx = w - 28.0;
    cairo_set_source_rgb(cr, 0.33, 0.33, 0.33);
    cairo_set_line_width(cr, 1.5);                            /* the wire */
    cairo_move_to(cr, pcx + 16, midy);
    cairo_line_to(cr, bdx - 14, midy);
    cairo_stroke(cr);
    /* this Mac: a little monitor + stand */
    cairo_set_source_rgb(cr, 0.16, 0.50, 0.82);
    cairo_set_line_width(cr, 1.8);
    cairo_rectangle(cr, pcx - 14, midy - 10, 27, 16);
    cairo_stroke(cr);
    cairo_move_to(cr, pcx - 3, midy + 6); cairo_line_to(cr, pcx - 6, midy + 11);
    cairo_move_to(cr, pcx + 2, midy + 6); cairo_line_to(cr, pcx + 5, midy + 11);
    cairo_move_to(cr, pcx - 8, midy + 11); cairo_line_to(cr, pcx + 7, midy + 11);
    cairo_stroke(cr);
    /* board / controller: a chip with pins (glows on a reply) */
    if (app->ping_pulse > 0) {
        cairo_set_source_rgba(cr, 0.30, 0.80, 0.42, 0.45);
        cairo_arc(cr, bdx, midy, 18, 0, 6.2832);
        cairo_fill(cr);
    }
    cairo_set_source_rgb(cr, 0.20, 0.66, 0.36);
    cairo_set_line_width(cr, 1.8);
    cairo_rectangle(cr, bdx - 9, midy - 9, 18, 18);
    cairo_stroke(cr);
    for (int i = 0; i < 3; i++) {
        double yy = midy - 5 + i * 5;
        cairo_move_to(cr, bdx - 9, yy); cairo_line_to(cr, bdx - 13, yy);
        cairo_move_to(cr, bdx + 9, yy); cairo_line_to(cr, bdx + 13, yy);
    }
    cairo_stroke(cr);
    /* one envelope per in-flight probe, sliding PC -> board */
    double x0 = pcx + 18, x1 = bdx - 16;
    for (int i = 0; i < app->ping_n_anim; i++) {
        double x = x0 + (x1 - x0) * app->ping_anim[i];
        cairo_set_source_rgb(cr, 0.96, 0.70, 0.14);
        cairo_rectangle(cr, x - 5.5, midy - 3.5, 11, 7);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.22, 0.15, 0.03);
        cairo_set_line_width(cr, 0.9);
        cairo_rectangle(cr, x - 5.5, midy - 3.5, 11, 7);
        cairo_stroke(cr);
        cairo_move_to(cr, x - 5.5, midy - 3.5);
        cairo_line_to(cr, x, midy + 0.5);
        cairo_line_to(cr, x + 5.5, midy - 3.5);
        cairo_stroke(cr);
    }
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    ctext(cr, pcx, midy - 15, "this Mac", 8, 0.5);
    ctext(cr, bdx, midy - 15, "board", 8, 0.5);
    ctext(cr, pcx, h - 3, HOST_IP, 9, 0.66);
    ctext(cr, bdx, h - 3, app->ping_target[0] ? app->ping_target : BOARD_IP,
          9, 0.66);
}

void ping_draw_spark(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                     gpointer user)
{
    (void)area;
    App *app = user;
    cairo_set_source_rgb(cr, 0.04, 0.04, 0.04);
    cairo_paint(cr);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    int n = app->ping_n_rtt;
    if (n == 0) {
        cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
        const char *msg = "no data yet -- click Ping to chart round-trip time";
        cairo_text_extents_t e;
        cairo_text_extents(cr, msg, &e);
        cairo_move_to(cr, (w - e.width) / 2.0, h / 2.0 + 4);
        cairo_show_text(cr, msg);
        return;
    }
    double pad_l = 38.0, pad_t = 4.0, pad_b = 13.0;
    double pw = w - pad_l - 6;
    double ph = h - pad_t - pad_b;
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    double hi = app->ping_rtts[0];
    for (int i = 0; i < n; i++) {
        if (app->ping_rtts[i] > hi) {
            hi = app->ping_rtts[i];
        }
    }
    double scale = hi > 0 ? hi : 1.0;
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    char lab[32];
    snprintf(lab, sizeof(lab), "%.0f ms", hi);
    cairo_move_to(cr, 2, pad_t + 8);
    cairo_show_text(cr, lab);
    cairo_move_to(cr, 2, pad_t + ph);
    cairo_show_text(cr, "0");
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, pad_l, pad_t + ph);
    cairo_line_to(cr, pad_l + pw, pad_t + ph);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0.54, 0.89, 0.20);
    cairo_set_line_width(cr, 1.5);
    for (int i = 0; i < n; i++) {
        double x = pad_l + pw * (n > 1 ? (double)i / (n - 1) : 0.5);
        double y = pad_t + ph - (app->ping_rtts[i] / scale) * ph;
        if (i) {
            cairo_line_to(cr, x, y);
        } else {
            cairo_move_to(cr, x, y);
        }
    }
    cairo_stroke(cr);
    for (int i = 0; i < n; i++) {
        double x = pad_l + pw * (n > 1 ? (double)i / (n - 1) : 0.5);
        double y = pad_t + ph - (app->ping_rtts[i] / scale) * ph;
        cairo_arc(cr, x, y, 2.0, 0, 6.2832);
        cairo_fill(cr);
    }
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    char foot[64];
    snprintf(foot, sizeof(foot), "packet 1..%d  (left = first)", n);
    cairo_move_to(cr, pad_l, h - 2);
    cairo_show_text(cr, foot);
}
