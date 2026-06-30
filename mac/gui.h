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

/* Wi-Fi reply-capture state (gui_wifi.c): which command's output we are
 * currently slicing off the live console stream (WIFI_CAP_NONE = passthrough). */
enum {
    WIFI_CAP_NONE = 0,
    WIFI_CAP_LIST,
    WIFI_CAP_STATUS,
    WIFI_CAP_IP,
    WIFI_CAP_PING,
    WIFI_CAP_NTP,
};

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
    gboolean    slip_armed;     /* self-syncing SLIP demux: END arms; a frame
                                 * starts only on a following IPv4 nibble (0x4N) */
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
    char        utf8_carry[8];         /* trailing partial multibyte char held */
    int         utf8_carry_n;          /* bytes valid in utf8_carry */

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
    GtkWidget     *slip_tx_lbl, *slip_rx_lbl;  /* per-frame SLIP activity LEDs */
    guint          slip_tx_src, slip_rx_src;   /* blink-off timeout handles */
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

    /* widgets: Wi-Fi panel (RP2350W) -- ports tcon/wifi.py + tcon/ui.py block */
    GtkWidget   *wifi_led;        /* lights-row dot: green/amber/red */
    GtkWidget   *wifi_ip_chip;    /* lights-row IP chip next to the dot */
    GtkWidget   *wifi_scan_btn;
    GtkWidget   *wifi_status_lbl; /* scan-row status line (_wifi_say) */
    GtkWidget   *wifi_list;       /* GtkListBox of scanned APs */
    GtkWidget   *wifi_ssid;       /* GtkEntry */
    GtkWidget   *wifi_pwd;        /* GtkPasswordEntry */
    GtkWidget   *wifi_wpa3;       /* GtkCheckButton -> connect3 */
    GtkWidget   *wifi_conn_btn;
    GtkWidget   *wifi_disc_btn;
    GtkWidget   *wifi_ip_lbl;     /* "on the network" readout */
    GtkWidget   *wifi_up_btn;     /* Go online */
    GtkWidget   *wifi_ping_btn;
    GtkWidget   *wifi_ping_t;     /* GtkEntry, default 8.8.8.8 */
    GtkWidget   *wifi_ntp_btn;    /* Time */
    GtkWidget   *wifi_net_lbl;    /* on-the-network status (_wifi_net_say) */
    GtkWidget   *wifi_pane;       /* wrapper box: shown only for RP2350-class ports */

    /* Wi-Fi capture/orchestration state (ports _wifi_init) */
    int          wifi_capture;    /* enum WIFI_CAP_* */
    GString     *wifi_linebuf;    /* RX accumulator, sliced on '\n' */
    GPtrArray   *wifi_aps;        /* of wifi_ap_t* (owned, g_free) */
    char         wifi_link[64];   /* parsed `wifi status` Link: value */
    int          wifi_poll;       /* status-poll counter (99 = stop) */
    char         wifi_ip[40];     /* live `ip` parse buffer */
    int          wifi_ip_tries;   /* DHCP-lease read retries */
    char         wifi_pingsum[160];
    char         wifi_ntp[160];
    gboolean     wifi_joined;     /* last known radio link state (drives LED) */
    char         wifi_ip_shown[40]; /* stable lease for the chip ("" = none) */
    gboolean     wifi_auto_up;    /* a join auto-runs Go online (init TRUE) */
    gboolean     wifi_board;      /* selected port is RP2350-class (gates the pane) */

    /* firmware build/flash bar */
    gboolean     bld_running;
    int          bld_step;             /* 0 clean, 1 build, 2 flash */
    int          bld_nflag;
    char        *bld_flagv[40];        /* g_strdup'd make flags */
    int          bld_nradios;
    GtkWidget   *bld_radios[8];        /* one MCU radio per board */
    GtkWidget   *bld_btn;
    GtkWidget   *bld_shell, *bld_net, *bld_basic, *bld_color, *bld_wifi, *bld_usb;
    GtkWidget   *bld_web;              /* web (HTTPS/TLS) firmware profile */
    GSubprocess *bld_proc;
    char         proj_dir[1024];       /* the tikuOS root (where make runs) */
    char         bld_board_key[32];
    int          bld_board_baud;
    int          bld_try;
    gboolean     bld_user_picked;      /* user chose an MCU -> stop auto-select */
    gboolean     bld_set_programmatic; /* guard: ignore our own radio toggles */

    /* /data file browser window */
    GtkWidget   *files_btn;
    GtkWidget   *files_win;
    GtkWidget   *files_list;           /* GtkListBox of filenames */
    GtkWidget   *files_usage;          /* header-bar usage subtitle */
    GtkWidget   *files_status;         /* action-bar status label */
    GtkWidget   *files_dl_btn;         /* Download (selection-gated) */
    GtkWidget   *files_del_btn;        /* Delete (selection-gated) */
    GtkWidget   *files_path_lbl;       /* header: the current /data path */
    GtkWidget   *files_up_btn;         /* header: go to the parent folder */
    char         files_cwd[256];       /* current sub-path under /data ("" = root) */
    char         files_xfer_name[256]; /* child path pending an async file dialog */

    /* startup splash */
    GtkWidget   *splash, *splash_load, *splash_prog;
    int          splash_ticks;
} App;

/* --- gui.c (shared helpers) --- */
void ser_write(App *app, const char *buf, size_t len);
void send_line(App *app, const char *line);
void set_status(App *app, const char *text, gboolean err);
void update_leds(App *app);
void console_append(App *app, const char *data, int len);
const char *baud_get_text(App *app);          /* selected toolbar baud as a string */
void baud_set_text(App *app, const char *s);  /* select the matching standard rate */

/* --- gui_net.c --- */
GtkWidget *build_netpanel(App *app);
void net_apply(App *app, gboolean active);
void net_down(App *app);
void net_on_ip_packet(App *app, const uint8_t *pkt, size_t len);
gboolean net_counters_tick(gpointer user);
void set_wifi_pane_visible(App *app, const char *plat);  /* RP2350-only WiFi pane */
void slip_blink(App *app, const char *dir);              /* "tx"/"rx" activity pulse */

/* --- gui_wifi.c --- */
GtkWidget *build_wifi_panel(App *app);    /* the Wi-Fi section of the side panel */
void wifi_feed(App *app, const char *text, int len);  /* console-stream tap */
void wifi_update_led(App *app);           /* refresh the lights-row Wi-Fi dot */
gboolean wifi_sync_cb(gpointer user);     /* post-connect quiet status sync */

/* --- gui.c (for the build bar) --- */
gboolean gui_autoconnect_step(App *app);  /* refresh + connect; TRUE if up */
void gui_disconnect(App *app);            /* free the port before flashing */

/* --- gui_build.c --- */
GtkWidget *build_buildbar(App *app);
void bld_autoselect(App *app);            /* select the attached board's family */
void bld_debug_dump(App *app);            /* smoke: print proj_dir + flags */

/* --- gui_splash.c --- */
void show_splash(App *app);               /* presents the main window when done */

/* --- gui.c (file-op console guard) --- */
int  files_pause_console(App *app);       /* remove the read-watch; fd or -1 */
void files_resume_console(App *app);      /* restore the read-watch */

/* --- gui_files.c --- */
void files_window_open(App *app);         /* open/raise the /data browser window */

/* --- gui_ping.c --- */
void ping_start(App *app, const char *target);
void ping_on_reply(App *app, const uint8_t *pkt, size_t len);
void ping_cancel(App *app);
void ping_draw_anim(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                    gpointer user);
void ping_draw_spark(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                     gpointer user);

#endif /* TIKUCONSOLE_GUI_H */
