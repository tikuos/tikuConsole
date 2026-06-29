/*
 * gui_wifi.c - the Wi-Fi panel for RP2350W boards.
 *
 * A faithful C/GTK4 port of the Linux tcon/wifi.py WiFiMixin (+ the WiFi
 * status light from tcon/leds.py and the on-the-network readout from
 * tcon/ui.py).  It drives the board's own `wifi`/`ip`/`ping`/`ntp` shell
 * commands and parses the replies straight off the live console stream:
 * wifi_feed() is called for every chunk of console text (see gui.c), and a
 * tiny capture state machine (app->wifi_capture) slices the reply we asked
 * for without disturbing what the user sees.
 *
 * Flow: Scan -> `wifi scan` then `wifi list` -> pick an SSID -> Connect ->
 * `wifi connect[3] <ssid> <pass>`, poll `wifi status` until joined, then
 * `wifi up` + `ip` (retry) for the DHCP lease.  Ping/Time run `ping`/`ntp`.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define AMBER "#e6b800"
#define RED   "#ff6b6b"
#define DIM   "#888888"

typedef struct {
    char ssid[64];
    char bssid[24];
    int  rssi;
    int  ch;
} wifi_ap_t;

/* --- forward decls (the timeout chain references itself) --- */
static void     wifi_line(App *app, const char *line);
static void     wifi_say(App *app, const char *text, int tone);
static void     wifi_net_say(App *app, const char *text, int tone);
static void     wifi_bring_online(App *app);
static gboolean wifi_do_list_cb(gpointer user);
static gboolean wifi_show_aps_cb(gpointer user);
static gboolean wifi_poll_status_cb(gpointer user);
static gboolean wifi_apply_status_cb(gpointer user);
static gboolean wifi_online_cb(gpointer user);
static gboolean wifi_read_ip_cb(gpointer user);
static gboolean wifi_apply_ip_cb(gpointer user);
static gboolean wifi_sync_apply_cb(gpointer user);
static gboolean wifi_apply_ping_cb(gpointer user);
static gboolean wifi_apply_ntp_cb(gpointer user);

/* tone: 0 = neutral (dim), 1 = ok (green), 2 = error (red) */
static const char *tone_col(int tone)
{
    return tone == 1 ? GREEN : tone == 2 ? RED : DIM;
}

/* ------------------------------------------------------------------------- */
/* Console tap: feed every chunk of console text through the capture machine */
/* ------------------------------------------------------------------------- */

/* drop CSI/SGR escape sequences (\x1b[ ... <letter>) before matching */
static void strip_ansi(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < outlen; ) {
        if (in[i] == 0x1b && in[i + 1] == '[') {
            i += 2;
            while (in[i] && !((in[i] >= 'A' && in[i] <= 'Z') ||
                              (in[i] >= 'a' && in[i] <= 'z')))
                i++;
            if (in[i]) i++;                 /* skip the final letter */
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = 0;
}

void wifi_feed(App *app, const char *text, int len)
{
    if (app->wifi_capture == WIFI_CAP_NONE)
        return;
    g_string_append_len(app->wifi_linebuf, text, len);

    char *nl;
    while ((nl = strchr(app->wifi_linebuf->str, '\n')) != NULL) {
        size_t linelen = (size_t)(nl - app->wifi_linebuf->str);
        char *raw = g_strndup(app->wifi_linebuf->str, linelen);
        g_string_erase(app->wifi_linebuf, 0, (gssize)(linelen + 1));

        char clean[512];
        strip_ansi(raw, clean, sizeof(clean));
        size_t cl = strlen(clean);
        while (cl && clean[cl - 1] == '\r')      /* rstrip CR (Python parity) */
            clean[--cl] = 0;
        wifi_line(app, clean);
        g_free(raw);
    }
}

/* read the integer immediately preceding `p` (skipping spaces); -1 if none */
static int back_int(const char *base, const char *p)
{
    const char *q = p;
    while (q > base && (q[-1] == ' ' || q[-1] == '\t')) q--;
    const char *end = q;
    while (q > base && q[-1] >= '0' && q[-1] <= '9') q--;
    if (q == end) return -1;
    return atoi(q);
}

static void wifi_line(App *app, const char *line)
{
    switch (app->wifi_capture) {

    case WIFI_CAP_LIST: {                         /* "  1  <bssid>  -91  1  SSID" */
        int idx, rssi, ch, consumed = 0;
        char bssid[32];
        if (sscanf(line, " %d %31s %d %d %n",
                   &idx, bssid, &rssi, &ch, &consumed) == 4 &&
            strlen(bssid) == 17 && bssid[2] == ':') {
            const char *ssid = line + consumed;
            while (*ssid == ' ' || *ssid == '\t') ssid++;
            char s[64];
            g_strlcpy(s, ssid, sizeof(s));
            size_t sl = strlen(s);
            while (sl && (s[sl - 1] == ' ' || s[sl - 1] == '\t')) s[--sl] = 0;
            if (sl) {
                wifi_ap_t *ap = g_new0(wifi_ap_t, 1);
                g_strlcpy(ap->ssid, s, sizeof(ap->ssid));
                g_strlcpy(ap->bssid, bssid, sizeof(ap->bssid));
                ap->rssi = rssi;
                ap->ch = ch;
                g_ptr_array_add(app->wifi_aps, ap);
            }
        }
        break;
    }

    case WIFI_CAP_STATUS:                          /* "Link: joined \"MyAP\"" */
        if (g_str_has_prefix(line, "Link:")) {
            const char *v = line + 5;
            while (*v == ' ') v++;
            g_strlcpy(app->wifi_link, v, sizeof(app->wifi_link));
        }
        break;

    case WIFI_CAP_IP: {                            /* "IPv4: 192.168.1.115" */
        const char *p = strstr(line, "IPv4:");
        if (p) {
            p += 5;
            while (*p == ' ') p++;
            g_strlcpy(app->wifi_ip, p, sizeof(app->wifi_ip));
            size_t l = strlen(app->wifi_ip);
            while (l && (app->wifi_ip[l - 1] == ' ' ||
                         app->wifi_ip[l - 1] == '\t')) app->wifi_ip[--l] = 0;
        }
        break;
    }

    case WIFI_CAP_PING:                            /* "--- 8.8.8.8 ping: 4 sent, 4 received ---" */
        if (strstr(line, "sent,") && strstr(line, "received")) {
            char s[160];
            g_strlcpy(s, line, sizeof(s));
            char *a = s, *b = s + strlen(s);       /* strip outer spaces + dashes */
            while (*a == ' ' || *a == '-') a++;
            while (b > a && (b[-1] == ' ' || b[-1] == '-')) *--b = 0;
            g_strlcpy(app->wifi_pingsum, a, sizeof(app->wifi_pingsum));
        }
        break;

    case WIFI_CAP_NTP: {                           /* "ntp: 2026-... UTC stratum 1" */
        const char *s = line;
        while (*s == ' ') s++;
        if (g_str_has_prefix(s, "ntp:")) {
            s += 4;
            while (*s == ' ') s++;
            g_strlcpy(app->wifi_ntp, s, sizeof(app->wifi_ntp));
        }
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Status light (lights row) + the two panel status lines                    */
/* ------------------------------------------------------------------------- */

void wifi_update_led(App *app)
{
    if (!app->wifi_led)
        return;
    gboolean joined = app->wifi_joined && app->ser_fd >= 0;
    gboolean haveip = app->wifi_ip_shown[0] != 0;
    const char *col = (joined && haveip) ? GREEN : joined ? AMBER : RED;

    char m[128];
    snprintf(m, sizeof(m),
             "<span foreground='%s'>\xe2\x97\x8f</span> WiFi", col);
    gtk_label_set_markup(GTK_LABEL(app->wifi_led), m);

    char chip[160];
    if (joined && haveip)
        snprintf(chip, sizeof(chip),
                 "<span foreground='%s' size='small'>%s</span>",
                 GREEN, app->wifi_ip_shown);
    else
        chip[0] = 0;
    gtk_label_set_markup(GTK_LABEL(app->wifi_ip_chip), chip);
}

static void wifi_say(App *app, const char *text, int tone)
{
    char *esc = g_markup_escape_text(text, -1);
    char m[640];
    snprintf(m, sizeof(m), "<span foreground='%s'>%s</span>",
             tone_col(tone), esc);
    gtk_label_set_markup(GTK_LABEL(app->wifi_status_lbl), m);
    g_free(esc);
}

static void wifi_net_say(App *app, const char *text, int tone)
{
    char *esc = g_markup_escape_text(text, -1);
    char m[640];
    snprintf(m, sizeof(m), "<span foreground='%s'>%s</span>",
             tone_col(tone), esc);
    gtk_label_set_markup(GTK_LABEL(app->wifi_net_lbl), m);
    g_free(esc);
}

static void wifi_bars(int rssi, char *buf, size_t len)
{
    int n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
    buf[0] = 0;
    for (int i = 0; i < 4; i++)
        g_strlcat(buf, i < n ? "\xe2\x96\x88" : "\xe2\x96\x91", len);
}

/* ------------------------------------------------------------------------- */
/* Scan: `wifi scan` (2.5s) -> `wifi list` (0.9s) -> de-dup, sort, show       */
/* ------------------------------------------------------------------------- */

static void on_wifi_scan(GtkWidget *w, gpointer user)
{
    (void)w;
    App *app = user;
    if (app->ser_fd < 0) {
        wifi_say(app, "connect to a board first", 2);
        return;
    }
    gtk_widget_set_sensitive(app->wifi_scan_btn, FALSE);
    wifi_say(app, "scanning\xe2\x80\xa6", 0);
    send_line(app, "wifi scan");
    g_timeout_add(2500, wifi_do_list_cb, app);
}

static gboolean wifi_do_list_cb(gpointer user)
{
    App *app = user;
    g_ptr_array_set_size(app->wifi_aps, 0);
    g_string_truncate(app->wifi_linebuf, 0);
    app->wifi_capture = WIFI_CAP_LIST;
    send_line(app, "wifi list");
    g_timeout_add(900, wifi_show_aps_cb, app);
    return G_SOURCE_REMOVE;
}

static void wifi_populate(App *app, GPtrArray *aps)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->wifi_list)))
        gtk_list_box_remove(GTK_LIST_BOX(app->wifi_list), child);

    for (guint i = 0; i < aps->len; i++) {
        wifi_ap_t *ap = aps->pdata[i];
        char bars[32];
        wifi_bars(ap->rssi, bars, sizeof(bars));
        char *esc = g_markup_escape_text(ap->ssid, -1);
        char m[256];
        snprintf(m, sizeof(m),
                 "%s  <span foreground='%s' size='small'>%s  ch%d  %ddBm</span>",
                 esc, DIM, bars, ap->ch, ap->rssi);
        GtkWidget *lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(lbl), m);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_widget_set_margin_start(lbl, 4);
        gtk_widget_set_margin_end(lbl, 4);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        g_object_set_data_full(G_OBJECT(row), "ssid",
                               g_strdup(ap->ssid), g_free);
        gtk_list_box_append(GTK_LIST_BOX(app->wifi_list), row);
        g_free(esc);
    }
}

static gboolean wifi_show_aps_cb(gpointer user)
{
    App *app = user;
    app->wifi_capture = WIFI_CAP_NONE;
    gtk_widget_set_sensitive(app->wifi_scan_btn, TRUE);

    /* de-dup by SSID (keep the strongest RSSI) */
    GPtrArray *uniq = g_ptr_array_new();
    for (guint i = 0; i < app->wifi_aps->len; i++) {
        wifi_ap_t *ap = app->wifi_aps->pdata[i];
        wifi_ap_t *found = NULL;
        for (guint j = 0; j < uniq->len; j++) {
            wifi_ap_t *u = uniq->pdata[j];
            if (!strcmp(u->ssid, ap->ssid)) { found = u; break; }
        }
        if (found) {
            if (ap->rssi > found->rssi) {
                found->rssi = ap->rssi;
                found->ch = ap->ch;
                g_strlcpy(found->bssid, ap->bssid, sizeof(found->bssid));
            }
        } else {
            g_ptr_array_add(uniq, ap);
        }
    }
    /* sort by RSSI descending (small N: simple selection sort) */
    for (guint i = 0; i < uniq->len; i++)
        for (guint j = i + 1; j < uniq->len; j++) {
            wifi_ap_t *a = uniq->pdata[i], *b = uniq->pdata[j];
            if (b->rssi > a->rssi) { uniq->pdata[i] = b; uniq->pdata[j] = a; }
        }

    wifi_populate(app, uniq);
    char msg[64];
    snprintf(msg, sizeof(msg), "%u network%s found",
             uniq->len, uniq->len == 1 ? "" : "s");
    wifi_say(app, msg, uniq->len ? 1 : 0);
    g_ptr_array_free(uniq, FALSE);       /* elements owned by app->wifi_aps */
    return G_SOURCE_REMOVE;
}

static void on_wifi_row(GtkListBox *box, GtkListBoxRow *row, gpointer user)
{
    (void)box;
    App *app = user;
    if (!row)
        return;
    const char *ssid = g_object_get_data(G_OBJECT(row), "ssid");
    if (ssid)
        gtk_editable_set_text(GTK_EDITABLE(app->wifi_ssid), ssid);
}

/* ------------------------------------------------------------------------- */
/* Connect: `wifi connect[3] <ssid> <pass>`, then poll `wifi status`          */
/* ------------------------------------------------------------------------- */

static void on_wifi_connect(GtkWidget *w, gpointer user)
{
    (void)w;
    App *app = user;
    if (app->ser_fd < 0) {
        wifi_say(app, "connect to a board first", 2);
        return;
    }
    const char *ssid = gtk_editable_get_text(GTK_EDITABLE(app->wifi_ssid));
    const char *psk  = gtk_editable_get_text(GTK_EDITABLE(app->wifi_pwd));
    if (!ssid || !ssid[0]) {
        wifi_say(app, "enter a network name (SSID)", 2);
        return;
    }
    if (strchr(ssid, ' ')) {
        wifi_say(app, "SSIDs with spaces aren't supported from here", 2);
        return;
    }
    const char *cmd =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(app->wifi_wpa3))
            ? "connect3" : "connect";
    char line[200];
    snprintf(line, sizeof(line), "wifi %s %s %s", cmd, ssid, psk ? psk : "");
    send_line(app, line);

    char say[96];
    snprintf(say, sizeof(say), "connecting to %s\xe2\x80\xa6", ssid);
    wifi_say(app, say, 0);
    app->wifi_poll = 0;
    g_timeout_add(2000, wifi_poll_status_cb, app);
}

static gboolean wifi_poll_status_cb(gpointer user)
{
    App *app = user;
    if (app->ser_fd < 0)
        return G_SOURCE_REMOVE;
    g_string_truncate(app->wifi_linebuf, 0);
    app->wifi_link[0] = 0;
    app->wifi_capture = WIFI_CAP_STATUS;
    send_line(app, "wifi status");
    g_timeout_add(600, wifi_apply_status_cb, app);
    app->wifi_poll++;
    return app->wifi_poll < 8 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean wifi_apply_status_cb(gpointer user)
{
    App *app = user;
    app->wifi_capture = WIFI_CAP_NONE;
    const char *link = app->wifi_link;
    if (g_str_has_prefix(link, "joined")) {
        char say[96];
        snprintf(say, sizeof(say), "\xe2\x9c\x93 %s", link);
        wifi_say(app, say, 1);
        app->wifi_poll = 99;
        app->wifi_joined = TRUE;
        wifi_update_led(app);
        if (app->wifi_auto_up)
            g_timeout_add(700, wifi_online_cb, app);
    } else if (g_str_has_prefix(link, "failed")) {
        wifi_say(app, "\xe2\x9c\x97 join failed \xe2\x80\x94 check the passphrase", 2);
        app->wifi_poll = 99;
        app->wifi_joined = FALSE;
        app->wifi_ip_shown[0] = 0;
        wifi_update_led(app);
    } else if (link[0]) {
        char say[96];
        snprintf(say, sizeof(say), "%s\xe2\x80\xa6", link);
        wifi_say(app, say, 0);
    }
    return G_SOURCE_REMOVE;
}

static void on_wifi_disconnect(GtkWidget *w, gpointer user)
{
    (void)w;
    App *app = user;
    if (app->ser_fd >= 0)
        send_line(app, "wifi disconnect");
    app->wifi_poll = 99;
    app->wifi_ip[0] = 0;
    app->wifi_joined = FALSE;
    app->wifi_ip_shown[0] = 0;
    wifi_update_led(app);
    gtk_label_set_markup(GTK_LABEL(app->wifi_ip_lbl),
                         "<span foreground='" DIM "'>not on the network</span>");
    wifi_say(app, "disconnect requested", 0);
}

/* ------------------------------------------------------------------------- */
/* Go online: `wifi up` -> `ip` (retry up to 4x) for the DHCP lease           */
/* ------------------------------------------------------------------------- */

static void wifi_bring_online(App *app)
{
    if (app->ser_fd < 0) {
        wifi_net_say(app, "connect to a board first", 2);
        return;
    }
    gtk_widget_set_sensitive(app->wifi_up_btn, FALSE);
    app->wifi_ip_tries = 0;
    wifi_net_say(app, "bringing the IP up\xe2\x80\xa6", 0);
    send_line(app, "wifi up");
    g_timeout_add(1500, wifi_read_ip_cb, app);
}

static void on_wifi_online(GtkWidget *w, gpointer user)
{
    (void)w;
    wifi_bring_online(user);
}

static gboolean wifi_online_cb(gpointer user)
{
    wifi_bring_online(user);
    return G_SOURCE_REMOVE;
}

static gboolean wifi_read_ip_cb(gpointer user)
{
    App *app = user;
    app->wifi_ip[0] = 0;
    g_string_truncate(app->wifi_linebuf, 0);
    app->wifi_capture = WIFI_CAP_IP;
    send_line(app, "ip");
    g_timeout_add(700, wifi_apply_ip_cb, app);
    return G_SOURCE_REMOVE;
}

static gboolean wifi_apply_ip_cb(gpointer user)
{
    App *app = user;
    app->wifi_capture = WIFI_CAP_NONE;
    app->wifi_ip_tries++;
    if (app->wifi_ip[0] && strcmp(app->wifi_ip, "0.0.0.0") &&
        strchr(app->wifi_ip, '.')) {
        g_strlcpy(app->wifi_ip_shown, app->wifi_ip, sizeof(app->wifi_ip_shown));
        wifi_update_led(app);
        char m[200];
        snprintf(m, sizeof(m),
                 "<span foreground='%s'>\xe2\x97\x8f online</span>  %s",
                 GREEN, app->wifi_ip_shown);
        gtk_label_set_markup(GTK_LABEL(app->wifi_ip_lbl), m);
        char say[96];
        snprintf(say, sizeof(say), "DHCP lease: %s", app->wifi_ip_shown);
        wifi_net_say(app, say, 1);
        gtk_widget_set_sensitive(app->wifi_up_btn, TRUE);
    } else if (app->wifi_ip_tries < 4) {
        g_timeout_add(1200, wifi_read_ip_cb, app);
    } else {
        app->wifi_ip_shown[0] = 0;
        wifi_update_led(app);
        gtk_label_set_markup(GTK_LABEL(app->wifi_ip_lbl),
                             "<span foreground='" RED "'>no DHCP lease yet</span>");
        wifi_net_say(app, "no IP yet \xe2\x80\x94 try Go online again", 2);
        gtk_widget_set_sensitive(app->wifi_up_btn, TRUE);
    }
    return G_SOURCE_REMOVE;
}

/* post-connect quiet sync: a cold-booted board may have auto-rejoined the
 * radio but hold no lease -- `wifi status`, and if joined, bring the IP up. */
gboolean wifi_sync_cb(gpointer user)
{
    App *app = user;
    if (!app->wifi_board)              /* non-RP2350 board has no `wifi` command */
        return G_SOURCE_REMOVE;
    if (app->ser_fd < 0 || app->wifi_capture != WIFI_CAP_NONE)
        return G_SOURCE_REMOVE;
    g_string_truncate(app->wifi_linebuf, 0);
    app->wifi_link[0] = 0;
    app->wifi_capture = WIFI_CAP_STATUS;
    send_line(app, "wifi status");
    g_timeout_add(700, wifi_sync_apply_cb, app);
    return G_SOURCE_REMOVE;
}

static gboolean wifi_sync_apply_cb(gpointer user)
{
    App *app = user;
    app->wifi_capture = WIFI_CAP_NONE;
    gboolean joined = g_str_has_prefix(app->wifi_link, "joined");
    app->wifi_joined = joined;
    wifi_update_led(app);
    if (joined)
        wifi_bring_online(app);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/* Ping / NTP (board-side, over Wi-Fi)                                        */
/* ------------------------------------------------------------------------- */

static void on_wifi_ping(GtkWidget *w, gpointer user)
{
    (void)w;
    App *app = user;
    if (app->ser_fd < 0) {
        wifi_net_say(app, "connect to a board first", 2);
        return;
    }
    const char *t = gtk_editable_get_text(GTK_EDITABLE(app->wifi_ping_t));
    char target[80];
    g_strlcpy(target, (t && t[0]) ? t : "8.8.8.8", sizeof(target));
    gtk_widget_set_sensitive(app->wifi_ping_btn, FALSE);
    app->wifi_pingsum[0] = 0;
    g_string_truncate(app->wifi_linebuf, 0);
    app->wifi_capture = WIFI_CAP_PING;
    char line[100];
    snprintf(line, sizeof(line), "ping %s", target);
    send_line(app, line);
    char say[120];
    snprintf(say, sizeof(say), "pinging %s\xe2\x80\xa6", target);
    wifi_net_say(app, say, 0);
    g_timeout_add(7000, wifi_apply_ping_cb, app);
}

static gboolean wifi_apply_ping_cb(gpointer user)
{
    App *app = user;
    app->wifi_capture = WIFI_CAP_NONE;
    gtk_widget_set_sensitive(app->wifi_ping_btn, TRUE);
    if (app->wifi_pingsum[0]) {
        const char *pr = strstr(app->wifi_pingsum, "received");
        int recv = pr ? back_int(app->wifi_pingsum, pr) : -1;
        gboolean ok = recv > 0;
        char say[200];
        snprintf(say, sizeof(say), "%s %s",
                 ok ? "\xe2\x9c\x93" : "\xe2\x9c\x97", app->wifi_pingsum);
        wifi_net_say(app, say, ok ? 1 : 2);
    } else {
        wifi_net_say(app, "no reply \xe2\x80\x94 is the board online?", 2);
    }
    return G_SOURCE_REMOVE;
}

static void on_wifi_ntp(GtkWidget *w, gpointer user)
{
    (void)w;
    App *app = user;
    if (app->ser_fd < 0) {
        wifi_net_say(app, "connect to a board first", 2);
        return;
    }
    gtk_widget_set_sensitive(app->wifi_ntp_btn, FALSE);
    app->wifi_ntp[0] = 0;
    g_string_truncate(app->wifi_linebuf, 0);
    app->wifi_capture = WIFI_CAP_NTP;
    send_line(app, "ntp");
    wifi_net_say(app, "fetching network time\xe2\x80\xa6", 0);
    g_timeout_add(9000, wifi_apply_ntp_cb, app);
}

static gboolean wifi_apply_ntp_cb(gpointer user)
{
    App *app = user;
    app->wifi_capture = WIFI_CAP_NONE;
    gtk_widget_set_sensitive(app->wifi_ntp_btn, TRUE);
    if (app->wifi_ntp[0] &&
        (strstr(app->wifi_ntp, "UTC") || strstr(app->wifi_ntp, "stratum"))) {
        char say[200];
        snprintf(say, sizeof(say), "\xf0\x9f\x95\x92 %s", app->wifi_ntp);
        wifi_net_say(app, say, 1);
    } else if (app->wifi_ntp[0]) {
        wifi_net_say(app, app->wifi_ntp, 2);
    } else {
        wifi_net_say(app, "no reply from the time server", 2);
    }
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/* Panel assembly                                                            */
/* ------------------------------------------------------------------------- */

static GtkWidget *wsection(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_margin_top(l, 6);
    PangoAttrList *a = pango_attr_list_new();
    pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(l), a);
    pango_attr_list_unref(a);
    return l;
}

GtkWidget *build_wifi_panel(App *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(box),
                   wsection("Wi-Fi (RP2350W: scan, join, go online)"));

    /* scan button + status line */
    GtkWidget *scanrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->wifi_scan_btn = gtk_button_new_with_label("Scan");
    g_signal_connect(app->wifi_scan_btn, "clicked",
                     G_CALLBACK(on_wifi_scan), app);
    gtk_box_append(GTK_BOX(scanrow), app->wifi_scan_btn);
    app->wifi_status_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->wifi_status_lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(app->wifi_status_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(app->wifi_status_lbl, TRUE);
    gtk_box_append(GTK_BOX(scanrow), app->wifi_status_lbl);
    gtk_box_append(GTK_BOX(box), scanrow);

    /* scrollable list of access points */
    app->wifi_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->wifi_list),
                                    GTK_SELECTION_SINGLE);
    g_signal_connect(app->wifi_list, "row-selected",
                     G_CALLBACK(on_wifi_row), app);
    GtkWidget *lsw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lsw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(lsw, -1, 116);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lsw), app->wifi_list);
    gtk_box_append(GTK_BOX(box), lsw);

    /* SSID entry */
    GtkWidget *ssrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(ssrow), gtk_label_new("SSID"));
    app->wifi_ssid = gtk_entry_new();
    gtk_widget_set_hexpand(app->wifi_ssid, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->wifi_ssid), "network name");
    gtk_box_append(GTK_BOX(ssrow), app->wifi_ssid);
    gtk_box_append(GTK_BOX(box), ssrow);

    /* passphrase + WPA3 toggle */
    GtkWidget *pwrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(pwrow), gtk_label_new("Pass"));
    app->wifi_pwd = gtk_password_entry_new();
    gtk_widget_set_hexpand(app->wifi_pwd, TRUE);
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(app->wifi_pwd),
                                          TRUE);
    gtk_box_append(GTK_BOX(pwrow), app->wifi_pwd);
    app->wifi_wpa3 = gtk_check_button_new_with_label("WPA3");
    gtk_widget_set_tooltip_text(app->wifi_wpa3,
                                "join with 'wifi connect3' (WPA3/SAE)");
    gtk_box_append(GTK_BOX(pwrow), app->wifi_wpa3);
    gtk_box_append(GTK_BOX(box), pwrow);

    /* connect / disconnect */
    GtkWidget *cdrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->wifi_conn_btn = gtk_button_new_with_label("Connect");
    gtk_widget_set_hexpand(app->wifi_conn_btn, TRUE);
    g_signal_connect(app->wifi_conn_btn, "clicked",
                     G_CALLBACK(on_wifi_connect), app);
    gtk_box_append(GTK_BOX(cdrow), app->wifi_conn_btn);
    app->wifi_disc_btn = gtk_button_new_with_label("Disconnect");
    g_signal_connect(app->wifi_disc_btn, "clicked",
                     G_CALLBACK(on_wifi_disconnect), app);
    gtk_box_append(GTK_BOX(cdrow), app->wifi_disc_btn);
    gtk_box_append(GTK_BOX(box), cdrow);

    /* on-the-network: IP readout + go-online / ping / time */
    gtk_box_append(GTK_BOX(box), wsection("On the network"));
    app->wifi_ip_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->wifi_ip_lbl), 0);
    gtk_label_set_wrap(GTK_LABEL(app->wifi_ip_lbl), TRUE);
    gtk_label_set_selectable(GTK_LABEL(app->wifi_ip_lbl), TRUE);
    gtk_label_set_markup(GTK_LABEL(app->wifi_ip_lbl),
        "<span foreground='" DIM "'>not on the network \xe2\x80\x94 "
        "Connect joins and brings the IP up</span>");
    gtk_box_append(GTK_BOX(box), app->wifi_ip_lbl);

    GtkWidget *arow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->wifi_up_btn = gtk_button_new_with_label("Go online");
    gtk_widget_set_tooltip_text(app->wifi_up_btn, "wifi up + read the DHCP lease");
    g_signal_connect(app->wifi_up_btn, "clicked",
                     G_CALLBACK(on_wifi_online), app);
    gtk_box_append(GTK_BOX(arow), app->wifi_up_btn);
    app->wifi_ping_btn = gtk_button_new_with_label("Ping");
    g_signal_connect(app->wifi_ping_btn, "clicked",
                     G_CALLBACK(on_wifi_ping), app);
    gtk_box_append(GTK_BOX(arow), app->wifi_ping_btn);
    app->wifi_ping_t = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(app->wifi_ping_t), "8.8.8.8");
    gtk_widget_set_hexpand(app->wifi_ping_t, TRUE);
    gtk_entry_set_max_length(GTK_ENTRY(app->wifi_ping_t), 32);
    g_signal_connect(app->wifi_ping_t, "activate",
                     G_CALLBACK(on_wifi_ping), app);
    gtk_box_append(GTK_BOX(arow), app->wifi_ping_t);
    app->wifi_ntp_btn = gtk_button_new_with_label("Time");
    gtk_widget_set_tooltip_text(app->wifi_ntp_btn, "fetch the time over NTP");
    g_signal_connect(app->wifi_ntp_btn, "clicked",
                     G_CALLBACK(on_wifi_ntp), app);
    gtk_box_append(GTK_BOX(arow), app->wifi_ntp_btn);
    gtk_box_append(GTK_BOX(box), arow);

    app->wifi_net_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->wifi_net_lbl), 0);
    gtk_label_set_wrap(GTK_LABEL(app->wifi_net_lbl), TRUE);
    gtk_label_set_selectable(GTK_LABEL(app->wifi_net_lbl), TRUE);
    gtk_box_append(GTK_BOX(box), app->wifi_net_lbl);

    return box;
}
