/*
 * gui_net.c - TikuConsole networking: the side-panel, the host utun bridge,
 * the rootless UDP relay, and pf-based NAT.
 *
 * Ports tcon/ui.py's networking pane, tcon/connection.py's TUN path, and
 * tcon/nat.py.  Two ways the board reaches the world:
 *
 *   * rootless: every off-link UDP datagram (DNS, NTP, ...) is relayed through
 *     ordinary host sockets and framed back over SLIP -- no privileges.
 *   * root: a utun device lets the macOS kernel route, and pf NAT (the iptables
 *     MASQUERADE equivalent) gives the board full internet incl. ICMP.
 *
 * The board's link MTU is tiny (128 B), so oversize DNS replies are trimmed to
 * their first A record before framing -- exactly what an IoT border router does.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <glib-unix.h>
#include <gtk/gtk.h>

#include "bridge.h"
#include "gui.h"
#include "packets.h"

#define BOARD_MTU 128

static const uint8_t HOST_BYTES[4]  = {172, 16, 7, 1};
static const uint8_t BOARD_BYTES[4] = {172, 16, 7, 2};

/* forward decls */
static gboolean on_nat_switch(GtkSwitch *sw, gboolean state, gpointer user);
static void on_slip_btn(GtkButton *b, gpointer user);
static void on_ping_clicked(GtkButton *b, gpointer user);
static void on_ping_activate(GtkEntry *e, gpointer user);
static int net_up(App *app);
static gboolean on_tun(gint fd, GIOCondition cond, gpointer user);
static int dns_fit_packet(App *app, const uint8_t *pkt, size_t len,
                          uint8_t *fit, size_t *fitlen);

/* Per-relay socket state for the GLib reply watch. */
typedef struct {
    App     *app;
    int      fd;
    guint    watch;
    guint    expire;
    uint8_t  dst_ip[4];
    uint16_t dst_port;
    uint8_t  src_ip[4];
    uint16_t src_port;
} relay_t;

/* ------------------------------------------------------------------------- */
/* Host interface lookup                                                     */
/* ------------------------------------------------------------------------- */

static void wan_iface(char *out, size_t len)
{
    out[0] = '\0';
    FILE *p = popen("route -n get 8.8.8.8 2>/dev/null | "
                    "awk '/interface:/{print $2; exit}'", "r");
    if (p) {
        if (fgets(out, (int)len, p)) {
            out[strcspn(out, "\r\n")] = '\0';
        }
        pclose(p);
    }
    if (!out[0]) {
        strlcpy(out, "en0", len);
    }
}

/* ------------------------------------------------------------------------- */
/* DNS adaptation for the constrained link                                   */
/* ------------------------------------------------------------------------- */

static int dns_skip_name(const uint8_t *b, size_t len, size_t pos, size_t *out)
{
    while (pos < len) {
        uint8_t n = b[pos];
        if ((n & 0xc0) == 0xc0) {           /* compression pointer (2 B) */
            if (pos + 2 <= len) {
                *out = pos + 2;
                return 1;
            }
            return 0;
        }
        if (n == 0) {                       /* root label ends the name */
            *out = pos + 1;
            return 1;
        }
        pos += 1 + n;
    }
    return 0;
}

/* Rebuild a DNS response as header + question + first A record.  Returns the
 * new length in out (<= 512), or 0 if it is not a shrinkable A-record answer. */
static size_t dns_trim(const uint8_t *dns, size_t len, uint8_t *out)
{
    if (len < 12) {
        return 0;
    }
    uint16_t flags = ((uint16_t)dns[2] << 8) | dns[3];
    if (!(flags & 0x8000) || (flags & 0x000f)) {  /* response, RCODE == 0 */
        return 0;
    }
    uint16_t qd = ((uint16_t)dns[4] << 8) | dns[5];
    uint16_t an = ((uint16_t)dns[6] << 8) | dns[7];
    if (qd != 1 || an < 1) {
        return 0;
    }
    size_t pos;
    if (!dns_skip_name(dns, len, 12, &pos) || pos + 4 > len) {
        return 0;
    }
    size_t q_end = pos + 4;                  /* + QTYPE + QCLASS */
    size_t qlen = q_end - 12;
    pos = q_end;
    for (uint16_t k = 0; k < an; k++) {
        size_t npos;
        if (!dns_skip_name(dns, len, pos, &npos) || npos + 10 > len) {
            return 0;
        }
        uint16_t rtype = ((uint16_t)dns[npos] << 8) | dns[npos + 1];
        uint16_t rclass = ((uint16_t)dns[npos + 2] << 8) | dns[npos + 3];
        const uint8_t *ttl = dns + npos + 4;
        uint16_t rdlen = ((uint16_t)dns[npos + 8] << 8) | dns[npos + 9];
        size_t rdata = npos + 10;
        if (rtype == 1 && rclass == 1 && rdlen == 4 && rdata + 4 <= len) {
            size_t o = 0;
            out[o++] = dns[0]; out[o++] = dns[1];         /* id */
            out[o++] = dns[2]; out[o++] = dns[3];         /* flags */
            out[o++] = 0; out[o++] = 1;                   /* QDCOUNT 1 */
            out[o++] = 0; out[o++] = 1;                   /* ANCOUNT 1 */
            out[o++] = 0; out[o++] = 0;                   /* NSCOUNT 0 */
            out[o++] = 0; out[o++] = 0;                   /* ARCOUNT 0 */
            memcpy(out + o, dns + 12, qlen); o += qlen;   /* the question */
            out[o++] = 0xc0; out[o++] = 0x0c;             /* name -> offset 12 */
            out[o++] = 0; out[o++] = 1;                   /* TYPE A */
            out[o++] = 0; out[o++] = 1;                   /* CLASS IN */
            memcpy(out + o, ttl, 4); o += 4;
            out[o++] = 0; out[o++] = 4;                   /* RDLENGTH 4 */
            memcpy(out + o, dns + rdata, 4); o += 4;      /* the address */
            return o;
        }
        pos = rdata + rdlen;
    }
    return 0;
}

/* If pkt is a board-bound DNS reply too big for the link MTU, write a trimmed
 * copy (first A record only) to fit/fitlen and return 1; else return 0. */
static int dns_fit_packet(App *app, const uint8_t *pkt, size_t len,
                          uint8_t *fit, size_t *fitlen)
{
    if (len <= BOARD_MTU || (pkt[0] >> 4) != 4 || pkt[9] != 17) {
        return 0;
    }
    size_t ihl = (size_t)(pkt[0] & 0x0f) * 4;
    if (len < ihl + 8) {
        return 0;
    }
    uint16_t sport = ((uint16_t)pkt[ihl] << 8) | pkt[ihl + 1];
    if (sport != 53) {                       /* not a UDP/53 (DNS) reply */
        return 0;
    }
    uint8_t trimmed[512];
    size_t tlen = dns_trim(pkt + ihl + 8, len - (ihl + 8), trimmed);
    if (tlen == 0) {
        return 0;
    }
    uint16_t dport = ((uint16_t)pkt[ihl + 2] << 8) | pkt[ihl + 3];
    uint8_t udp[600];
    size_t ul = build_udp(pkt + 12, 53, pkt + 16, dport, trimmed, tlen, udp);
    *fitlen = build_ip(pkt + 12, pkt + 16, 17, udp, ul, 0, fit);
    char m[160];
    snprintf(m, sizeof(m), "[relay] DNS reply %zuB > %dB MTU -- trimmed to "
             "first A record (%zuB)\n", len, BOARD_MTU, *fitlen);
    console_append(app, m, -1);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Rootless UDP relay (no TUN / no root)                                     */
/* ------------------------------------------------------------------------- */

static void relay_icmp_hint(App *app, const uint8_t *dst_ip)
{
    gint64 now = g_get_monotonic_time();
    if (now - app->icmp_hint_t < 5000000) {  /* ~1 per 5 s */
        return;
    }
    app->icmp_hint_t = now;
    char m[220];
    snprintf(m, sizeof(m),
        "[relay] ICMP to %u.%u.%u.%u needs root -- ping works only with the "
        "utun/NAT bridge; relaunch with sudo (UDP services like ntp/dns work "
        "rootless).\n", dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
    console_append(app, m, -1);
}

static gboolean relay_reply(gint fd, GIOCondition cond, gpointer user)
{
    (void)cond;
    relay_t *r = user;
    App *app = r->app;
    uint8_t reply[2048];
    ssize_t rn = recv(fd, reply, sizeof(reply), 0);
    if (rn > 0) {
        uint8_t udp[2080], ip[2120], fit[4608];
        size_t ul = build_udp(r->dst_ip, r->dst_port, r->src_ip, r->src_port,
                              reply, (size_t)rn, udp);
        size_t pl = build_ip(r->dst_ip, r->src_ip, 17, udp, ul, 0, ip);
        size_t fitlen;
        const uint8_t *out = dns_fit_packet(app, ip, pl, fit, &fitlen) ? fit : ip;
        if (out == ip) {
            fitlen = pl;
        }
        uint8_t enc[4608];
        size_t el = slip_encode(out, fitlen, enc);
        ser_write(app, (const char *)enc, el);
        app->fr_out++;
        app->by_out += (unsigned)fitlen;
        char m[120];
        snprintf(m, sizeof(m), "[relay] %u.%u.%u.%u:%u -> board (%zdB)\n",
                 r->dst_ip[0], r->dst_ip[1], r->dst_ip[2], r->dst_ip[3],
                 r->dst_port, rn);
        console_append(app, m, -1);
    }
    if (r->expire) {
        g_source_remove(r->expire);
    }
    close(r->fd);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static gboolean relay_expire(gpointer user)
{
    relay_t *r = user;
    if (r->watch) {
        g_source_remove(r->watch);
    }
    close(r->fd);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static void relay_udp(App *app, const uint8_t *pkt, size_t len)
{
    if (len < 20 || (pkt[0] >> 4) != 4) {
        return;
    }
    if (memcmp(pkt + 16, HOST_BYTES, 4) == 0) {
        return;                              /* for the host, not the world */
    }
    if (pkt[9] == 1) {                       /* ICMP -- needs root */
        relay_icmp_hint(app, pkt + 16);
        return;
    }
    if (pkt[9] != 17 || len < 28) {
        return;                              /* only UDP rides this relay */
    }
    size_t ihl = (size_t)(pkt[0] & 0x0f) * 4;
    if (len < ihl + 8) {
        return;
    }
    uint16_t src_port = ((uint16_t)pkt[ihl] << 8) | pkt[ihl + 1];
    uint16_t dst_port = ((uint16_t)pkt[ihl + 2] << 8) | pkt[ihl + 3];
    const uint8_t *payload = pkt + ihl + 8;
    size_t paylen = len - (ihl + 8);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return;
    }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(dst_port);
    memcpy(&sa.sin_addr, pkt + 16, 4);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        send(s, payload, paylen, 0) < 0) {
        char m[100];
        snprintf(m, sizeof(m), "[relay] %u.%u.%u.%u:%u unreachable\n",
                 pkt[16], pkt[17], pkt[18], pkt[19], dst_port);
        console_append(app, m, -1);
        close(s);
        return;
    }
    char m[100];
    snprintf(m, sizeof(m), "[relay] board -> %u.%u.%u.%u:%u (%zuB)\n",
             pkt[16], pkt[17], pkt[18], pkt[19], dst_port, paylen);
    console_append(app, m, -1);

    relay_t *r = g_new0(relay_t, 1);
    r->app = app;
    r->fd = s;
    memcpy(r->dst_ip, pkt + 16, 4);
    r->dst_port = dst_port;
    memcpy(r->src_ip, pkt + 12, 4);
    r->src_port = src_port;
    r->watch = g_unix_fd_add(s, G_IO_IN, relay_reply, r);
    r->expire = g_timeout_add_seconds(6, relay_expire, r);
}

/* ------------------------------------------------------------------------- */
/* Packet dispatch (board -> host)                                           */
/* ------------------------------------------------------------------------- */

void net_on_ip_packet(App *app, const uint8_t *pkt, size_t len)
{
    app->fr_in++;
    app->by_in += (unsigned)len;
    if (app->utun_fd >= 0) {
        utun_write(app->utun_fd, pkt, len);  /* root path: kernel routes it */
    } else {
        relay_udp(app, pkt, len);            /* rootless UDP->internet relay */
    }
    if (app->ping_active) {
        ping_on_reply(app, pkt, len);        /* in-app rootless pinger */
    }
}

/* ------------------------------------------------------------------------- */
/* Host utun bridge (root)                                                   */
/* ------------------------------------------------------------------------- */

static gboolean on_tun(gint fd, GIOCondition cond, gpointer user)
{
    (void)cond;
    App *app = user;
    uint8_t buf[BRIDGE_MTU + 4];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 4) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return G_SOURCE_CONTINUE;
        }
        return (n < 0) ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
    }
    uint8_t *pkt = buf + 4;                   /* strip the 4-byte AF header */
    size_t plen = (size_t)(n - 4);
    /* Only relay IPv4 unicast to the board; drop the kernel's multicast /
     * broadcast / IPv6 chatter so the tiny board is never flooded. */
    if (plen >= 20 && (pkt[0] >> 4) == 4 && memcmp(pkt + 16, BOARD_BYTES, 4) == 0) {
        uint8_t fit[4608];
        size_t fitlen;
        const uint8_t *out = dns_fit_packet(app, pkt, plen, fit, &fitlen)
                             ? fit : pkt;
        if (out == pkt) {
            fitlen = plen;
        }
        uint8_t enc[4608];
        size_t el = slip_encode(out, fitlen, enc);
        ser_write(app, (const char *)enc, el);
        app->fr_out++;
        app->by_out += (unsigned)fitlen;
    }
    return G_SOURCE_CONTINUE;
}

static int net_up(App *app)
{
    char name[32] = {0};
    int fd = utun_open(name, sizeof(name));
    if (fd < 0) {
        set_status(app, "network setup failed: could not create utun (need "
                        "sudo)", TRUE);
        return 0;
    }
    app->utun_fd = fd;
    strlcpy(app->utun_name, name, sizeof(app->utun_name));
    bridge_run("ifconfig %s inet %s %s up", name, HOST_IP, BOARD_IP);
    bridge_run("route -q -n add -net %s -interface %s 2>/dev/null", SUBNET, name);
    app->utun_watch = g_unix_fd_add(fd, G_IO_IN, on_tun, app);
    char l[96];
    snprintf(l, sizeof(l), "%s: up  %s <-> %s", name, HOST_IP, BOARD_IP);
    gtk_label_set_text(GTK_LABEL(app->tun_lbl), l);
    return 1;
}

void net_down(App *app)
{
    if (app->nat_on && app->nat_sw) {
        gtk_switch_set_active(GTK_SWITCH(app->nat_sw), FALSE);  /* drops pf */
    }
    if (app->utun_watch) {
        g_source_remove(app->utun_watch);
        app->utun_watch = 0;
    }
    if (app->utun_fd >= 0) {
        bridge_run("ifconfig %s down 2>/dev/null", app->utun_name);
        close(app->utun_fd);
        app->utun_fd = -1;
    }
    if (app->tun_lbl) {
        gtk_label_set_text(GTK_LABEL(app->tun_lbl), "utun: down");
    }
}

/* ------------------------------------------------------------------------- */
/* Networking apply (shared by the switch + connect)                         */
/* ------------------------------------------------------------------------- */

static gboolean auto_slip_cb(gpointer user)
{
    send_line((App *)user, "slip on");       /* explicit enable (idempotent) */
    return G_SOURCE_REMOVE;
}

void net_apply(App *app, gboolean active)
{
    if (!active) {
        /* keep the side panel up while connected (the Wi-Fi controls live
         * there); only hide it when there is no board attached. */
        gtk_widget_set_visible(app->netpanel, app->ser_fd >= 0);
        gtk_widget_set_visible(app->net_hint, FALSE);
        ping_cancel(app);
        send_line(app, "slip off");          /* console-only (idempotent) */
        net_down(app);
        app->net_mode = FALSE;
        gtk_widget_set_sensitive(app->slip_btn, FALSE);
        gtk_widget_set_sensitive(app->nat_sw, FALSE);
        console_append(app, "[tikuconsole] networking off -- console-only.\n",
                       -1);
        return;
    }
    gtk_widget_set_visible(app->netpanel, TRUE);
    gtk_widget_set_sensitive(app->slip_btn, TRUE);   /* SLIP needs no host root */
    if (geteuid() == 0 && net_up(app)) {
        app->net_mode = TRUE;
        gtk_widget_set_visible(app->net_hint, FALSE);
        gtk_widget_set_sensitive(app->nat_sw, TRUE);
        console_append(app, "[tikuconsole] networking on; enabling SLIP on the "
                            "board...\n", -1);
        g_timeout_add(400, auto_slip_cb, app);
        return;
    }
    /* No host utun (not root, or setup failed) -- SLIP + ping still work. */
    app->net_mode = FALSE;
    gtk_widget_set_sensitive(app->nat_sw, FALSE);
    const char *hint;
    if (geteuid() != 0) {
        hint = "\xe2\x9a\xa0 host utun/NAT bridge needs sudo. SLIP + board ping "
               "work without it; relaunch with sudo for the full bridge.";
        set_status(app, "Networking pane shown -- SLIP + board ping work now; "
                        "host utun/NAT needs sudo.", FALSE);
    } else {
        hint = "\xe2\x9a\xa0 utun setup failed -- see status above";
    }
    char mk[300];
    snprintf(mk, sizeof(mk),
             "<span foreground='#ff6b6b' weight='bold'>%s</span>", hint);
    gtk_label_set_markup(GTK_LABEL(app->net_hint), mk);
    gtk_widget_set_visible(app->net_hint, TRUE);
}

/* ------------------------------------------------------------------------- */
/* Panel callbacks                                                           */
/* ------------------------------------------------------------------------- */

static void on_slip_btn(GtkButton *b, gpointer user)
{
    (void)b;
    send_line((App *)user, "slip");
}

static gboolean on_nat_switch(GtkSwitch *sw, gboolean state, gpointer user)
{
    (void)sw;
    App *app = user;
    char wan[64];
    wan_iface(wan, sizeof(wan));
    if (state) {
        /* macOS pf NAT: the iptables MASQUERADE equivalent.  pf is disabled by
         * default on a dev Mac, so loading our rule + enabling pf is reversible
         * with `pfctl -d`. */
        bridge_run("sysctl -w net.inet.ip.forwarding=1 >/dev/null 2>&1");
        bridge_run("printf 'nat on %s from %s to any -> (%s)\\n' | "
                   "pfctl -f - 2>/dev/null", wan, SUBNET, wan);
        bridge_run("pfctl -e 2>/dev/null");
        char m[160];
        snprintf(m, sizeof(m),
                 "[nat] ON via %s  (ip.forwarding + pf nat)\n", wan);
        console_append(app, m, -1);
        char st[140];
        snprintf(st, sizeof(st),
                 "NAT on via %s -- ping 8.8.8.8 from the board", wan);
        set_status(app, st, FALSE);
        app->nat_on = TRUE;
    } else {
        bridge_run("pfctl -d 2>/dev/null");
        console_append(app, "[nat] OFF\n", -1);
        set_status(app, "NAT off", FALSE);
        app->nat_on = FALSE;
    }
    update_leds(app);
    return FALSE;
}

static void on_ping_clicked(GtkButton *b, gpointer user)
{
    (void)b;
    App *app = user;
    ping_start(app, gtk_editable_get_text(GTK_EDITABLE(app->ping_entry)));
}

static void on_ping_activate(GtkEntry *e, gpointer user)
{
    (void)e;
    App *app = user;
    ping_start(app, gtk_editable_get_text(GTK_EDITABLE(app->ping_entry)));
}

gboolean net_counters_tick(gpointer user)
{
    App *app = user;
    if (app->cnt_lbl) {
        char m[128];
        snprintf(m, sizeof(m), "frames in/out: %u/%u   bytes: %u/%u",
                 app->fr_in, app->fr_out, app->by_in, app->by_out);
        gtk_label_set_text(GTK_LABEL(app->cnt_lbl), m);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------- */
/* Panel assembly                                                            */
/* ------------------------------------------------------------------------- */

static GtkWidget *section(const char *text)
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

GtkWidget *build_netpanel(App *app)
{
    GtkWidget *nbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(nbox, 340, -1);

    /* Wi-Fi (RP2350W) rides above the SLIP/IP networking controls. */
    gtk_box_append(GTK_BOX(nbox), build_wifi_panel(app));
    gtk_box_append(GTK_BOX(nbox),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(nbox), section("Networking (SLIP/IP over the wire)"));
    app->net_hint = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(app->net_hint), 0);
    gtk_label_set_wrap(GTK_LABEL(app->net_hint), TRUE);
    gtk_widget_set_visible(app->net_hint, FALSE);
    gtk_box_append(GTK_BOX(nbox), app->net_hint);

    app->slip_btn = gtk_button_new_with_label("Toggle SLIP on board");
    gtk_widget_set_sensitive(app->slip_btn, FALSE);
    g_signal_connect(app->slip_btn, "clicked", G_CALLBACK(on_slip_btn), app);
    gtk_box_append(GTK_BOX(nbox), app->slip_btn);

    app->tun_lbl = gtk_label_new("utun: down");
    gtk_label_set_xalign(GTK_LABEL(app->tun_lbl), 0);
    gtk_box_append(GTK_BOX(nbox), app->tun_lbl);

    app->cnt_lbl = gtk_label_new("frames in/out: 0/0   bytes: 0/0");
    gtk_label_set_xalign(GTK_LABEL(app->cnt_lbl), 0);
    gtk_box_append(GTK_BOX(nbox), app->cnt_lbl);

    gtk_box_append(GTK_BOX(nbox), section("Internet (NAT: board \xe2\x86\x92 "
                                          "internet)"));
    GtkWidget *natb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(natb), gtk_label_new("enable"));
    app->nat_sw = gtk_switch_new();
    gtk_widget_set_valign(app->nat_sw, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(app->nat_sw, FALSE);
    g_signal_connect(app->nat_sw, "state-set", G_CALLBACK(on_nat_switch), app);
    gtk_box_append(GTK_BOX(natb), app->nat_sw);
    gtk_box_append(GTK_BOX(nbox), natb);

    gtk_box_append(GTK_BOX(nbox), section("Ping (host kernel \xe2\x86\x92 via "
                                          "utun)"));
    GtkWidget *pb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    app->ping_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(app->ping_entry), BOARD_IP);
    gtk_widget_set_hexpand(app->ping_entry, TRUE);
    g_signal_connect(app->ping_entry, "activate",
                     G_CALLBACK(on_ping_activate), app);
    gtk_box_append(GTK_BOX(pb), app->ping_entry);
    gtk_box_append(GTK_BOX(pb), gtk_label_new("\xc3\x97"));
    app->ping_spin = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->ping_spin), 5);
    gtk_widget_set_tooltip_text(app->ping_spin, "number of ping packets to send");
    gtk_box_append(GTK_BOX(pb), app->ping_spin);
    app->ping_btn = gtk_button_new_with_label("Ping");
    g_signal_connect(app->ping_btn, "clicked", G_CALLBACK(on_ping_clicked), app);
    gtk_box_append(GTK_BOX(pb), app->ping_btn);
    gtk_box_append(GTK_BOX(nbox), pb);

    app->ping_anim_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app->ping_anim_area), 66);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->ping_anim_area),
                                   ping_draw_anim, app, NULL);
    gtk_widget_set_tooltip_text(app->ping_anim_area,
        "this Mac --packets--> board (animated while pinging)");
    gtk_box_append(GTK_BOX(nbox), app->ping_anim_area);

    app->ping_stats = gtk_label_new("idle -- enter an address and click Ping");
    gtk_label_set_xalign(GTK_LABEL(app->ping_stats), 0);
    gtk_label_set_selectable(GTK_LABEL(app->ping_stats), TRUE);
    gtk_label_set_wrap(GTK_LABEL(app->ping_stats), TRUE);
    gtk_box_append(GTK_BOX(nbox), app->ping_stats);

    GtkWidget *cap = gtk_label_new("round-trip time per packet (taller = "
                                   "slower):");
    gtk_label_set_xalign(GTK_LABEL(cap), 0);
    gtk_widget_add_css_class(cap, "dim-label");
    gtk_box_append(GTK_BOX(nbox), cap);

    app->spark = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app->spark), 74);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->spark),
                                   ping_draw_spark, app, NULL);
    gtk_box_append(GTK_BOX(nbox), app->spark);

    GtkWidget *psw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(psw), 120);
    gtk_widget_set_vexpand(psw, TRUE);
    app->ping_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(app->ping_view, FALSE);
    gtk_text_view_set_monospace(app->ping_view, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(app->ping_view), "console");
    app->ping_buf = gtk_text_view_get_buffer(app->ping_view);
    app->ping_ok = gtk_text_buffer_create_tag(app->ping_buf, NULL,
        "foreground", GREEN, NULL);
    app->ping_bad = gtk_text_buffer_create_tag(app->ping_buf, NULL,
        "foreground", "#ff6b6b", NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(psw),
                                  GTK_WIDGET(app->ping_view));
    gtk_box_append(GTK_BOX(nbox), psw);
    return nbox;
}
