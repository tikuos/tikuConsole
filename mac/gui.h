/*
 * gui.h - shared state + cross-module declarations for the TikuConsole GUI.
 *
 * The GTK app is split the way the Linux tcon/ package is, one concern per
 * file: gui.c owns the window/console/serial, gui_net.c the networking panel +
 * utun bridge + UDP relay + NAT, gui_ping.c the in-app pinger + its charts.
 * They share one App instance, so methods stay plain function calls on it.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKUCONSOLE_GUI_H
#define TIKUCONSOLE_GUI_H

#include <stdint.h>
#include <gtk/gtk.h>

#include "ports.h"

#define VERSION  "0.01"
#define GREEN    "#8ae234"
#define HOST_IP  "172.16.7.1"
#define BOARD_IP "172.16.7.2"
#define SUBNET   "172.16.7.0/24"

#define PING_MAX 128            /* ping count is capped at 100 by the spinner */

typedef struct App {
    GtkApplication *app;
    GtkWindow      *win;

    /* serial link */
    int    ser_fd;
    guint  ser_watch;

    /* host utun bridge (root) */
    int    utun_fd;
    guint  utun_watch;
    char   utun_name[32];
    gboolean net_mode;             /* networking pane active this session */

    /* port list */
    port_info_t ports[PORTS_MAX];
    int         n_ports;
    char        port_path[256];

    /* SLIP demux state */
    gboolean    in_frame;
    GByteArray *frame;

    /* status lights */
    gboolean slip_on, nat_on;

    /* byte/frame counters */
    unsigned fr_in, fr_out, by_in, by_out;
    gint64   icmp_hint_t;          /* rate-limit the "ICMP needs root" note */

    /* in-app ICMP-over-SLIP ping (rootless) */
    gboolean ping_active;
    guint16  ping_ident;
    gint64   ping_send_t[PING_MAX];    /* monotonic us per seq */
    gboolean ping_pending[PING_MAX];
    double   ping_rtts[PING_MAX];
    int      ping_n_rtt;
    int      ping_sent, ping_recv;
    char     ping_target[64];
    int      ping_i, ping_n;
    double   ping_anim[256];           /* in-flight packet icons: progress 0..1 */
    int      ping_n_anim;
    guint    ping_anim_src;
    int      ping_pulse;               /* board glyph glows briefly on a reply */

    /* console ANSI/SGR decode state */
    GString    *ansi_pending;
    GString    *slip_scan;
    gboolean    sgr_bold, sgr_dim;
    int         sgr_fg;                /* 0 = default, else 30..37 */
    GtkTextTag *tag_fg[8];
    GtkTextTag *tag_bold, *tag_dim;

    /* widgets: connection bar + console */
    GtkWidget     *port_dd;
    GtkWidget     *platform_lbl;
    GtkWidget     *baud;
    GtkWidget     *net_sw;
    GtkWidget     *connect_btn;
    GtkWidget     *status;
    GtkWidget     *usb_led, *slip_led, *nat_led;
    GtkTextView   *cview;
    GtkTextBuffer *cbuf;
    GtkAdjustment *cadj;
    gboolean       follow;
    GtkWidget     *paned;

    /* widgets: networking panel */
    GtkWidget     *netpanel;
    GtkWidget     *net_hint;
    GtkWidget     *slip_btn;
    GtkWidget     *tun_lbl;
    GtkWidget     *cnt_lbl;
    GtkWidget     *nat_sw;
    GtkWidget     *ping_entry;
    GtkWidget     *ping_spin;
    GtkWidget     *ping_btn;
    GtkWidget     *ping_anim_area;
    GtkWidget     *ping_stats;
    GtkWidget     *spark;
    GtkTextView   *ping_view;
    GtkTextBuffer *ping_buf;
    GtkTextTag    *ping_ok, *ping_bad;

    /* firmware build/flash bar */
    gboolean     bld_running;
    int          bld_step;             /* 0 clean, 1 build, 2 flash */
    int          bld_nflag;
    char        *bld_flagv[40];        /* g_strdup'd make flags */
    int          bld_nradios;
    GtkWidget   *bld_radios[8];        /* one MCU radio per board */
    GtkWidget   *bld_btn;
    GtkWidget   *bld_shell, *bld_net, *bld_basic, *bld_color;
    GSubprocess *bld_proc;
    char         proj_dir[1024];       /* the tikuOS root (where make runs) */
    char         bld_board_key[32];
    int          bld_board_baud;
    int          bld_try;
} App;

/* --- gui.c (shared helpers) --- */
void ser_write(App *app, const char *buf, size_t len);
void send_line(App *app, const char *line);
void set_status(App *app, const char *text, gboolean err);
void update_leds(App *app);
void console_append(App *app, const char *data, int len);

/* --- gui_net.c --- */
GtkWidget *build_netpanel(App *app);
void net_apply(App *app, gboolean active);
void net_down(App *app);
void net_on_ip_packet(App *app, const uint8_t *pkt, size_t len);
gboolean net_counters_tick(gpointer user);

/* --- gui.c (for the build bar) --- */
gboolean gui_autoconnect_step(App *app);  /* refresh + connect; TRUE if up */
void gui_disconnect(App *app);            /* free the port before flashing */

/* --- gui_build.c --- */
GtkWidget *build_buildbar(App *app);
void bld_debug_dump(App *app);            /* smoke: print proj_dir + flags */

/* --- gui_ping.c --- */
void ping_start(App *app, const char *target);
void ping_on_reply(App *app, const uint8_t *pkt, size_t len);
void ping_cancel(App *app);
void ping_draw_anim(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                    gpointer user);
void ping_draw_spark(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                     gpointer user);

#endif /* TIKUCONSOLE_GUI_H */
