/* network.c - connection status, DNS provider and Wi-Fi selection.
 *
 * - Wi-Fi name / band / signal / scan list : libnl-3 + nl80211 (generic netlink)
 * - Connection type : /proc/net/route + /sys/class/net (Wi-Fi, Ethernet, VPN
 *   badge over the physical link)
 * - Ping + packet loss to the default gateway : background thread (ICMP)
 * - DNS provider : nameserver lines of /etc/resolv.conf
 * - Known networks : wpa_supplicant control socket ($SYSTRAY_WPA_DIR)
 * - Buttons and network rows run the setup script:
 *       script WIFI <interface> <essid> <address> <open|wep|wpa|wpa2|wpa3>
 *                   (secret only in the environment: WIFI_PASSWORD)
 *       script DNS  <dhcp|cloudflare|google|custom> [interface]
 *       script REBIND <interface>
 *   (script path: $SYSTRAY_NET_SCRIPT or NET_SCRIPT_DEFAULT)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <netinet/ip_icmp.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/netlink.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#define POP_W    420
#define MAXAP    32
#define MAXKNOWN 32
#define LIST_ROWS 6                       /* visible "other networks" rows */

enum { DNS_DHCP, DNS_CLOUDFLARE, DNS_GOOGLE, DNS_CUSTOM };
static const char *dns_names[] = {"DHCP", "Cloudflare", "Google", "Custom"};
static const char *dns_args[]  = {"dhcp", "cloudflare", "google", "custom"};

enum { ENC_OPEN, ENC_WEP, ENC_WPA, ENC_WPA2, ENC_WPA3 };
static const char *const enc_arg[]   = {"open", "wep", "wpa", "wpa2", "wpa3"};
static const char *const enc_label[] = {"Open network (no encryption)",
                                        "WEP encryption (weak)", "WPA encryption",
                                        "WPA2 encryption", "WPA3 encryption"};

typedef struct {
    char ssid[64];
    int dbm, freq, enc, connected, known;
    char bssid[18];                       /* address of the strongest AP */
} Ap;

static struct {
    char iface[32], ip[32], gw[32];
    int up;                               /* default route present            */
    int wifi;                             /* iface is wireless                */
    int ether;                            /* iface is Ethernet (not tun/tap)  */
    char vpn[32];                         /* VPN interface on top, or ""      */
    char ssid[64];
    int freq, dbm, have_dbm;
    double rx_rate, tx_rate;              /* bytes/s                          */
    double rx_total, tx_total;            /* bytes since boot                 */
    int dns;
    int no_ns;                            /* resolv.conf has no nameserver    */
    Ap aps[MAXAP];
    int nap;
} N;

static char known[MAXKNOWN][64];
static int nknown;
static int popup_open_flag, scroll_off, hover_row = -1, tick_n, help_hover;
static int nons_hover;                     /* "no nameserver" help on DHCP */
static int enc_hover = -1, hover_mx, hover_my;   /* encryption icon tooltip */

/* ------------------------------------------------------------------ */
/* Ping thread                                                         */
/* ------------------------------------------------------------------ */

#define HIST 20
static pthread_t ping_th;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int ping_run;
static char ping_gw[32];
static double ping_ms = -1;
static int hist[HIST], hist_n, hist_pos;     /* 1 = ok, 0 = lost */

static uint16_t cksum(const void *p, int len)
{
    const uint16_t *w = p;
    uint32_t s = 0;
    for (; len > 1; len -= 2) s += *w++;
    if (len) s += *(const uint8_t *)w;
    s = (s >> 16) + (s & 0xffff);
    s += s >> 16;
    return (uint16_t)~s;
}

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ICMP echo through an unprivileged datagram socket. Returns rtt or -1. */
static double probe_icmp(const char *gw, int seq)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s < 0) return -2;
    struct sockaddr_in to = {.sin_family = AF_INET};
    if (inet_pton(AF_INET, gw, &to.sin_addr) != 1) { close(s); return -1; }

    struct icmphdr h;
    memset(&h, 0, sizeof h);
    h.type = ICMP_ECHO;
    h.un.echo.sequence = htons(seq);
    h.checksum = cksum(&h, sizeof h);

    double t0 = mono(), rtt = -1;
    if (sendto(s, &h, sizeof h, 0, (struct sockaddr *)&to, sizeof to) == sizeof h) {
        for (;;) {
            double left = 1000 - (mono() - t0);
            if (left <= 0) break;
            struct pollfd pf = {s, POLLIN, 0};
            if (poll(&pf, 1, (int)left) <= 0) break;
            char buf[128];
            ssize_t r = recv(s, buf, sizeof buf, 0);
            if (r >= (ssize_t)sizeof(struct icmphdr)) {
                struct icmphdr *rh = (struct icmphdr *)buf;
                if (rh->type == ICMP_ECHOREPLY && ntohs(rh->un.echo.sequence) == (uint16_t)seq) {
                    rtt = mono() - t0;
                    break;
                }
            }
        }
    }
    close(s);
    return rtt;
}

/* Fallback when ICMP sockets are not permitted: use ping(8). */
static double probe_exec(const char *gw)
{
    char cmd[128];
    struct in_addr a;
    if (inet_pton(AF_INET, gw, &a) != 1) return -1;
    snprintf(cmd, sizeof cmd, "ping -n -c 1 -W 1 %s 2>/dev/null", gw);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char line[256];
    double rtt = -1;
    while (fgets(line, sizeof line, f)) {
        char *t = strstr(line, "time=");
        if (t) { rtt = atof(t + 5); break; }
    }
    pclose(f);
    return rtt;
}

static void *ping_thread(void *arg)
{
    (void)arg;
    int seq = 0;
    char last_gw[32] = "";
    while (ping_run) {
        char gw[32];
        pthread_mutex_lock(&mu);
        snprintf(gw, sizeof gw, "%s", ping_gw);
        pthread_mutex_unlock(&mu);
        if (!gw[0]) { usleep(500000); continue; }

        double t0 = mono();
        double rtt = probe_icmp(gw, ++seq);
        if (rtt == -2) rtt = probe_exec(gw);

        pthread_mutex_lock(&mu);
        if (strcmp(gw, last_gw)) {              /* new gateway: reset stats */
            snprintf(last_gw, sizeof last_gw, "%s", gw);
            hist_n = hist_pos = 0;
            ping_ms = -1;
        }
        hist[hist_pos] = rtt >= 0;
        hist_pos = (hist_pos + 1) % HIST;
        if (hist_n < HIST) hist_n++;
        if (rtt >= 0) ping_ms = rtt;
        pthread_mutex_unlock(&mu);

        double rest = 1000 - (mono() - t0);
        if (rest > 0) usleep((useconds_t)(rest * 1000));
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* /proc, /sys, resolv.conf                                            */
/* ------------------------------------------------------------------ */

/* Ethernet link layer: /sys/class/net/<if>/type is ARPHRD_ETHER (1).  That
 * is also true for Wi-Fi (told apart by nl80211, see show_wired) and for TAP
 * devices (they have tun_flags); WireGuard / tun (65534) and PPP (512) are
 * not Ethernet.  Bridges and bonds over Ethernet count as Ethernet. */
static int is_ethernet(const char *ifn)
{
    char p[128], b[16] = "";
    FILE *f;
    snprintf(p, sizeof p, "/sys/class/net/%s/type", ifn);
    if (!(f = fopen(p, "r"))) return 0;
    if (!fgets(b, sizeof b, f)) b[0] = 0;
    fclose(f);
    if (atoi(b) != 1) return 0;
    snprintf(p, sizeof p, "/sys/class/net/%s/tun_flags", ifn);
    return access(p, F_OK) != 0;
}

/* VPN tunnel: tun / WireGuard (type ARPHRD_NONE, 65534) or a TUN/TAP device
 * (has tun_flags; covers OpenVPN in tap mode). */
static int is_vpn(const char *ifn)
{
    char p[128], b[16] = "";
    FILE *f;
    snprintf(p, sizeof p, "/sys/class/net/%s/tun_flags", ifn);
    if (access(p, F_OK) == 0) return 1;
    snprintf(p, sizeof p, "/sys/class/net/%s/type", ifn);
    if (!(f = fopen(p, "r"))) return 0;
    if (!fgets(b, sizeof b, f)) b[0] = 0;
    fclose(f);
    return atoi(b) == 65534;
}

/* First VPN interface that is administratively up (IFF_UP).  Used when no
 * VPN route is visible in the main table: wg-quick routes everything through
 * its own table plus a policy rule, which /proc/net/route does not show. */
static int vpn_up_iface(char *out, size_t n)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d))) {
        /* interface names are < IFNAMSIZ (16); anything else is not one */
        if (e->d_name[0] == '.' || strlen(e->d_name) >= IFNAMSIZ) continue;
        if (!is_vpn(e->d_name)) continue;
        char p[300], b[32] = "";
        FILE *f;
        snprintf(p, sizeof p, "/sys/class/net/%s/flags", e->d_name);
        if (!(f = fopen(p, "r"))) continue;
        if (!fgets(b, sizeof b, f)) b[0] = 0;
        fclose(f);
        if (strtoul(b, NULL, 16) & 1) {
            snprintf(out, n, "%.15s", e->d_name);       /* IFNAMSIZ - 1 */
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* The physical link (lowest-metric default route that is not a VPN) is what
 * the widget describes: Wi-Fi signal, ping to its gateway, traffic.  A VPN
 * on top of it only sets N.vpn, shown as a badge on the icon. */
static void read_route(void)
{
    char iface[32] = "", gw[32] = "", vpn[32] = "", vpn_def[32] = "";
    long best_metric = -1, vpn_metric = -1;
    FILE *f = fopen("/proc/net/route", "r");
    if (f) {
        char line[256];
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        while (fgets(line, sizeof line, f)) {
            char ifn[32];
            unsigned dest, gwv, flags, refcnt, use, metric, mask;
            /* Iface Destination Gateway Flags RefCnt Use Metric Mask ...;
             * addresses are raw network-order words */
            if (sscanf(line, "%31s %x %x %x %u %u %u %x", ifn, &dest, &gwv, &flags,
                       &refcnt, &use, &metric, &mask) != 8) continue;
            if (!(flags & 1)) continue;                        /* RTF_UP */
            if (is_vpn(ifn)) {
                /* default route, or OpenVPN "def1": 0.0.0.0/1 + 128.0.0.0/1;
                 * tunnels often have no gateway (RTF_GATEWAY not set) */
                int full = mask == 0 && dest == 0;
                int def1 = mask == htonl(0x80000000u) &&
                           (dest == 0 || dest == htonl(0x80000000u));
                if (full && (vpn_metric < 0 || (long)metric < vpn_metric)) {
                    vpn_metric = metric;
                    snprintf(vpn_def, sizeof vpn_def, "%s", ifn);
                }
                if ((full || def1) && !vpn[0]) snprintf(vpn, sizeof vpn, "%s", ifn);
                continue;
            }
            if (dest != 0 || mask != 0 || !(flags & 2)) continue;
            if (best_metric < 0 || (long)metric < best_metric) {
                best_metric = metric;
                snprintf(iface, sizeof iface, "%s", ifn);
                struct in_addr a = {.s_addr = gwv};
                inet_ntop(AF_INET, &a, gw, sizeof gw);
            }
        }
        fclose(f);
    }
    if (vpn_def[0]) snprintf(vpn, sizeof vpn, "%s", vpn_def);
    if (!vpn[0]) vpn_up_iface(vpn, sizeof vpn);

    /* only a VPN default route (no physical one visible): describe the VPN
     * interface itself, as before */
    if (!iface[0] && vpn_def[0]) snprintf(iface, sizeof iface, "%s", vpn_def);

    N.up = iface[0] != 0;
    if (N.up) {
        snprintf(N.iface, sizeof N.iface, "%s", iface);
        snprintf(N.gw, sizeof N.gw, "%s", gw);
    } else {
        N.gw[0] = 0;
    }
    N.ether = N.up && is_ethernet(iface);
    /* a VPN only counts while there is a connection under it */
    snprintf(N.vpn, sizeof N.vpn, "%s", N.up ? vpn : "");
    pthread_mutex_lock(&mu);
    snprintf(ping_gw, sizeof ping_gw, "%s", N.gw);
    pthread_mutex_unlock(&mu);
}

static void read_ip(void)
{
    N.ip[0] = 0;
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) < 0) return;
    for (p = ifa; p; p = p->ifa_next)
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET &&
            !strcmp(p->ifa_name, N.iface)) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr,
                      N.ip, sizeof N.ip);
            break;
        }
    freeifaddrs(ifa);
}

static void read_traffic(void)
{
    static double prx = -1, ptx = -1, pt;
    char p[128], b[32];
    double rx = 0, tx = 0;
    FILE *f;
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/rx_bytes", N.iface);
    if ((f = fopen(p, "r"))) { if (fgets(b, sizeof b, f)) rx = atof(b); fclose(f); }
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/tx_bytes", N.iface);
    if ((f = fopen(p, "r"))) { if (fgets(b, sizeof b, f)) tx = atof(b); fclose(f); }
    double t = mono() / 1000.0;
    if (prx >= 0 && t > pt && rx >= prx && tx >= ptx) {
        N.rx_rate = (rx - prx) / (t - pt);
        N.tx_rate = (tx - ptx) / (t - pt);
    } else {
        N.rx_rate = N.tx_rate = 0;
    }
    prx = rx; ptx = tx; pt = t;
    N.rx_total = rx;
    N.tx_total = tx;
}

static int is_local_ns(const char *ns)
{
    struct in_addr a;
    if (inet_pton(AF_INET, ns, &a) == 1) {
        uint32_t h = ntohl(a.s_addr);
        if ((h >> 24) == 127 || (h >> 24) == 10 || (h >> 20) == 0xAC1 ||
            (h >> 16) == 0xC0A8 || (h >> 16) == 0xA9FE)
            return 1;
        return 0;
    }
    return !strncasecmp(ns, "fe80", 4) || !strncasecmp(ns, "fd", 2) ||
           !strncasecmp(ns, "fc", 2) || !strcmp(ns, "::1");
}

static int detect_dns(void)
{
    char ns[8][64];
    int n = 0;
    N.no_ns = 1;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return DNS_DHCP;
    char line[256];
    while (fgets(line, sizeof line, f) && n < 8) {
        char addr[64];
        const char *p = line + strspn(line, " \t");
        if (*p == '#' || *p == ';') continue;     /* commented out: not active */
        if (sscanf(p, "nameserver %63s", addr) == 1) {
            char *pct = strchr(addr, '%');           /* strip zone id */
            if (pct) *pct = 0;
            snprintf(ns[n++], 64, "%s", addr);
        }
    }
    fclose(f);
    N.no_ns = !n;
    if (!n) return DNS_DHCP;

    int cf = 0, gg = 0, local = 0, other = 0, custom = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(ns[i], "127.0.0.1")) custom++;  /* what "Custom" writes */
        else if (!strcmp(ns[i], "1.1.1.1") || !strcmp(ns[i], "1.0.0.1")) cf++;
        else if (!strcmp(ns[i], "8.8.8.8") || !strcmp(ns[i], "8.8.4.4")) gg++;
        else if (is_local_ns(ns[i])) local++;
        else other++;
    }
    if (custom == n) return DNS_CUSTOM;
    if (cf == n) return DNS_CLOUDFLARE;
    if (gg == n) return DNS_GOOGLE;
    if (local == n) return DNS_DHCP;        /* router / other local stub resolver */
    return DNS_CUSTOM;
}

static int add_known(const char *s)
{
    if (!*s || nknown >= MAXKNOWN) return 0;
    snprintf(known[nknown++], 64, "%.63s", s);
    return 1;
}

static int valid_ifname(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.'))
            return 0;
    return 1;
}

/* Talk to wpa_supplicant's control interface (the same unix datagram socket
 * wpa_cli uses).  The socket directory is $SYSTRAY_WPA_DIR, default
 * /var/run/wpa_supplicant; the socket is <dir>/<iface>.
 * Returns the reply length (NUL terminated in buf) or -1 with err filled. */
#define WPA_DIR_DEFAULT "/var/run/wpa_supplicant"

static int wpa_request(const char *iface, const char *cmd, char *buf, size_t size,
                       char *err, size_t errsz)
{
    const char *dir = getenv("SYSTRAY_WPA_DIR");
    if (!dir || !*dir) dir = WPA_DIR_DEFAULT;

    struct sockaddr_un srv = {.sun_family = AF_UNIX}, loc = {.sun_family = AF_UNIX};
    if (snprintf(srv.sun_path, sizeof srv.sun_path, "%s/%s", dir, iface)
            >= (int)sizeof srv.sun_path) {
        snprintf(err, errsz, "socket path too long");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { snprintf(err, errsz, "socket: %s", strerror(errno)); return -1; }

    static int seq;
    snprintf(loc.sun_path, sizeof loc.sun_path, "/tmp/systray_wpa_%d_%d",
             (int)getpid(), seq++);
    unlink(loc.sun_path);
    if (bind(fd, (struct sockaddr *)&loc, sizeof loc) < 0) {
        snprintf(err, errsz, "bind %s: %s", loc.sun_path, strerror(errno));
        close(fd);
        return -1;
    }

    int n = -1;
    struct timeval tv = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&srv, sizeof srv) < 0) {
        snprintf(err, errsz, "connect %s: %s", srv.sun_path, strerror(errno));
    } else if (send(fd, cmd, strlen(cmd), 0) < 0) {
        snprintf(err, errsz, "send to %s: %s", srv.sun_path, strerror(errno));
    } else {
        for (;;) {
            n = (int)recv(fd, buf, size - 1, 0);
            if (n < 0) {
                snprintf(err, errsz, "recv from %s: %s", srv.sun_path, strerror(errno));
                break;
            }
            buf[n] = 0;
            if (n > 0 && buf[0] == '<') continue;       /* unsolicited event */
            break;
        }
    }
    close(fd);
    unlink(loc.sun_path);
    return n;
}

/* Known (configured) networks come from the LIST_NETWORKS control command
 * (lines: network id <TAB> ssid <TAB> bssid <TAB> flags).  If the socket is not
 * usable, fall back to ~/.config/systray/known_networks (one SSID per line). */
static void load_known(void)
{
    nknown = 0;
    int ok = 0;

    if (valid_ifname(N.iface)) {
        static char reply[8192];
        char err[256] = "";
        int n = wpa_request(N.iface, "LIST_NETWORKS", reply, sizeof reply, err, sizeof err);
        if (n >= 0) {
            ok = 1;
            for (char *line = strtok(reply, "\n"); line; line = strtok(NULL, "\n")) {
                if (!isdigit((unsigned char)line[0])) continue;   /* header */
                char *t1 = strchr(line, '\t');
                if (!t1) continue;
                char *ssid = t1 + 1, *t2 = strchr(ssid, '\t');
                if (t2) *t2 = 0;
                add_known(ssid);
            }
        } else {
            const char *dir = getenv("SYSTRAY_WPA_DIR");
            fprintf(stderr, "systray: wpa_supplicant LIST_NETWORKS failed for '%s' "
                    "(socket dir %s): %s\n", N.iface,
                    dir && *dir ? dir : WPA_DIR_DEFAULT, err);
        }
    }
    if (ok) return;

    const char *home = getenv("HOME");
    if (!home) return;
    char path[320];
    snprintf(path, sizeof path, "%s/.config/systray/known_networks", home);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] != '#') add_known(line);
    }
    fclose(f);
}
static int is_known(const char *ssid)
{
    for (int i = 0; i < nknown; i++) if (!strcmp(known[i], ssid)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* libnl / nl80211                                                     */
/* ------------------------------------------------------------------ */

static struct nl_sock *nls;
static int nl_id = -1;

static int err_cb(struct sockaddr_nl *a, struct nlmsgerr *e, void *arg)
{ (void)a; *(int *)arg = e->error; return NL_STOP; }
static int fin_cb(struct nl_msg *m, void *arg)
{ (void)m; *(int *)arg = 0; return NL_SKIP; }
static int ack_cb(struct nl_msg *m, void *arg)
{ (void)m; *(int *)arg = 0; return NL_STOP; }

static struct nl_msg *mk_msg(int cmd, int flags, int ifindex)
{
    struct nl_msg *m = nlmsg_alloc();
    genlmsg_put(m, NL_AUTO_PORT, NL_AUTO_SEQ, nl_id, 0, flags, cmd, 0);
    if (ifindex) nla_put_u32(m, NL80211_ATTR_IFINDEX, ifindex);
    return m;
}

static int nl_call(struct nl_msg *msg, int (*h)(struct nl_msg *, void *), void *arg)
{
    int err = 1;
    struct nl_cb *cb = nl_cb_alloc(NL_CB_DEFAULT);
    nl_cb_err(cb, NL_CB_CUSTOM, err_cb, &err);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, fin_cb, &err);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_cb, &err);
    if (h) nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, h, arg);
    int r = nl_send_auto(nls, msg);
    if (r < 0) err = r;
    else while (err > 0) {
        r = nl_recvmsgs(nls, cb);
        if (r < 0 && err > 0) err = r;
    }
    nl_cb_put(cb);
    nlmsg_free(msg);
    return err;
}

static int parse_attrs(struct nl_msg *msg, struct nlattr **tb)
{
    struct genlmsghdr *g = nlmsg_data(nlmsg_hdr(msg));
    return nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(g, 0),
                     genlmsg_attrlen(g, 0), NULL);
}

typedef struct { int idx; char name[32]; char ssid[64]; int freq; } WIf;
static WIf wifs[4];
static int nwif;

static int h_iface(struct nl_msg *msg, void *arg)
{
    (void)arg;
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    parse_attrs(msg, tb);
    if (!tb[NL80211_ATTR_IFINDEX] || !tb[NL80211_ATTR_IFNAME] || nwif >= 4)
        return NL_SKIP;
    if (tb[NL80211_ATTR_IFTYPE] &&
        nla_get_u32(tb[NL80211_ATTR_IFTYPE]) != NL80211_IFTYPE_STATION)
        return NL_SKIP;
    WIf *w = &wifs[nwif++];
    memset(w, 0, sizeof *w);
    w->idx = nla_get_u32(tb[NL80211_ATTR_IFINDEX]);
    snprintf(w->name, sizeof w->name, "%s", nla_get_string(tb[NL80211_ATTR_IFNAME]));
    if (tb[NL80211_ATTR_SSID]) {
        int l = nla_len(tb[NL80211_ATTR_SSID]);
        if (l > 63) l = 63;
        memcpy(w->ssid, nla_data(tb[NL80211_ATTR_SSID]), l);
        w->ssid[l] = 0;
    }
    if (tb[NL80211_ATTR_WIPHY_FREQ]) w->freq = nla_get_u32(tb[NL80211_ATTR_WIPHY_FREQ]);
    return NL_SKIP;
}

static int h_station(struct nl_msg *msg, void *arg)
{
    struct nlattr *tb[NL80211_ATTR_MAX + 1], *si[NL80211_STA_INFO_MAX + 1];
    parse_attrs(msg, tb);
    if (!tb[NL80211_ATTR_STA_INFO]) return NL_SKIP;
    if (nla_parse_nested(si, NL80211_STA_INFO_MAX, tb[NL80211_ATTR_STA_INFO], NULL))
        return NL_SKIP;
    if (si[NL80211_STA_INFO_SIGNAL]) {
        *(int *)arg = (int8_t)nla_get_u8(si[NL80211_STA_INFO_SIGNAL]);
        N.have_dbm = 1;
    }
    return NL_SKIP;
}

/* Flags collected from the information elements of a BSS */
enum { F_WPA = 1, F_RSN = 2, F_PSK = 4, F_SAE = 8, F_EAP = 16, F_OWE = 32 };

static void parse_rsn(const uint8_t *p, int rem, int *flags)
{
    *flags |= F_RSN;
    if (rem < 8) return;
    p += 6; rem -= 6;                               /* version + group cipher */
    int pc = p[0] | (p[1] << 8);
    p += 2; rem -= 2;
    if (rem < pc * 4 + 2) return;
    p += pc * 4; rem -= pc * 4;                     /* pairwise ciphers */
    int ac = p[0] | (p[1] << 8);
    p += 2; rem -= 2;
    for (int i = 0; i < ac && rem >= 4; i++, p += 4, rem -= 4) {
        if (p[0] != 0x00 || p[1] != 0x0f || p[2] != 0xac) continue;
        switch (p[3]) {
        case 1: case 3: case 5:   *flags |= F_EAP; break;   /* 802.1X */
        case 2: case 4: case 6:   *flags |= F_PSK; break;   /* PSK    */
        case 8: case 9:           *flags |= F_SAE; break;   /* SAE    */
        case 18:                  *flags |= F_OWE; break;   /* OWE    */
        }
    }
}

static void parse_ies(const uint8_t *ie, int len, char *ssid, size_t n, int *flags)
{
    while (len >= 2) {
        int id = ie[0], l = ie[1];
        if (l + 2 > len) break;
        if (id == 0) {
            size_t c = (size_t)l < n - 1 ? (size_t)l : n - 1;
            memcpy(ssid, ie + 2, c);
            ssid[c] = 0;
            for (size_t i = 0; i < c; i++)
                if (ssid[i] == 0) { ssid[0] = 0; break; }   /* hidden network */
        } else if (id == 48) {
            parse_rsn(ie + 2, l, flags);                     /* RSN (WPA2/WPA3) */
        } else if (id == 221 && l >= 4 && ie[2] == 0x00 && ie[3] == 0x50 &&
                   ie[4] == 0xf2 && ie[5] == 1) {
            *flags |= F_WPA;                                 /* WPA (legacy) */
        }
        ie += l + 2;
        len -= l + 2;
    }
}

static int classify(int flags, int privacy)
{
    if (flags & F_RSN) {
        if ((flags & F_OWE) && !(flags & (F_PSK | F_SAE | F_EAP))) return ENC_OPEN;
        return (flags & F_SAE) && !(flags & F_PSK) ? ENC_WPA3 : ENC_WPA2;
    }
    if (flags & F_WPA) return ENC_WPA;
    return privacy ? ENC_WEP : ENC_OPEN;
}
typedef struct { Ap aps[MAXAP]; int n; char assoc[64]; int assoc_freq; } Scan;

static int h_scan(struct nl_msg *msg, void *arg)
{
    Scan *sc = arg;
    struct nlattr *tb[NL80211_ATTR_MAX + 1], *bss[NL80211_BSS_MAX + 1];
    parse_attrs(msg, tb);
    if (!tb[NL80211_ATTR_BSS] ||
        nla_parse_nested(bss, NL80211_BSS_MAX, tb[NL80211_ATTR_BSS], NULL))
        return NL_SKIP;

    char ssid[64] = "";
    int flags = 0;
    if (bss[NL80211_BSS_INFORMATION_ELEMENTS])
        parse_ies(nla_data(bss[NL80211_BSS_INFORMATION_ELEMENTS]),
                  nla_len(bss[NL80211_BSS_INFORMATION_ELEMENTS]), ssid, sizeof ssid, &flags);
    int privacy = bss[NL80211_BSS_CAPABILITY] &&
                  (nla_get_u16(bss[NL80211_BSS_CAPABILITY]) & 0x10);
    int enc = classify(flags, privacy);
    if (!ssid[0]) return NL_SKIP;

    char bssid[18] = "";
    if (bss[NL80211_BSS_BSSID] && nla_len(bss[NL80211_BSS_BSSID]) == 6) {
        const uint8_t *m = nla_data(bss[NL80211_BSS_BSSID]);
        snprintf(bssid, sizeof bssid, "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    int dbm = bss[NL80211_BSS_SIGNAL_MBM] ? (int)((int32_t)nla_get_u32(bss[NL80211_BSS_SIGNAL_MBM]) / 100) : -100;
    int freq = bss[NL80211_BSS_FREQUENCY] ? (int)nla_get_u32(bss[NL80211_BSS_FREQUENCY]) : 0;
    int assoc = bss[NL80211_BSS_STATUS] &&
                nla_get_u32(bss[NL80211_BSS_STATUS]) == NL80211_BSS_STATUS_ASSOCIATED;
    if (assoc) {
        snprintf(sc->assoc, sizeof sc->assoc, "%s", ssid);
        sc->assoc_freq = freq;
    }

    for (int i = 0; i < sc->n; i++)            /* de-duplicate by SSID */
        if (!strcmp(sc->aps[i].ssid, ssid)) {
            if (dbm > sc->aps[i].dbm) {
                sc->aps[i].dbm = dbm; sc->aps[i].freq = freq; sc->aps[i].enc = enc;
                snprintf(sc->aps[i].bssid, sizeof sc->aps[i].bssid, "%s", bssid);
            }
            return NL_SKIP;
        }
    if (sc->n < MAXAP) {
        Ap *a = &sc->aps[sc->n++];
        memset(a, 0, sizeof *a);
        snprintf(a->ssid, sizeof a->ssid, "%s", ssid);
        a->dbm = dbm; a->freq = freq; a->enc = enc;
        snprintf(a->bssid, sizeof a->bssid, "%s", bssid);
    }
    return NL_SKIP;
}

static void trigger_scan(int ifindex)
{
    struct nl_msg *m = mk_msg(NL80211_CMD_TRIGGER_SCAN, 0, ifindex);
    struct nl_msg *ssids = nlmsg_alloc();
    nla_put(ssids, 1, 0, "");                       /* wildcard SSID */
    nla_put_nested(m, NL80211_ATTR_SCAN_SSIDS, ssids);
    nlmsg_free(ssids);
    nl_call(m, NULL, NULL);                          /* may need CAP_NET_ADMIN */
}

static int cmp_ap(const void *a, const void *b)
{
    return ((const Ap *)b)->dbm - ((const Ap *)a)->dbm;
}

static void scan_dump(int ifindex)
{
    static Scan sc;
    memset(&sc, 0, sizeof sc);
    load_known();
    nl_call(mk_msg(NL80211_CMD_GET_SCAN, NLM_F_DUMP, ifindex), h_scan, &sc);

    if (N.ssid[0] == 0 && sc.assoc[0]) {             /* kernel w/o SSID in iface info */
        snprintf(N.ssid, sizeof N.ssid, "%s", sc.assoc);
        N.freq = sc.assoc_freq;
    }
    qsort(sc.aps, sc.n, sizeof sc.aps[0], cmp_ap);
    N.nap = 0;
    for (int i = 0; i < sc.n; i++) {
        Ap a = sc.aps[i];
        a.connected = N.ssid[0] && !strcmp(a.ssid, N.ssid);
        a.known = a.connected || is_known(a.ssid);
        N.aps[N.nap++] = a;
    }
}

static void wifi_update(void)
{
    N.ssid[0] = 0;
    N.freq = 0;
    N.have_dbm = 0;
    N.wifi = 0;
    if (!nls) return;

    nwif = 0;
    nl_call(mk_msg(NL80211_CMD_GET_INTERFACE, NLM_F_DUMP, 0), h_iface, NULL);
    WIf *w = NULL;
    for (int i = 0; i < nwif; i++)
        if (N.up && !strcmp(wifs[i].name, N.iface)) w = &wifs[i];
    if (!w && !N.up && nwif) w = &wifs[0];      /* offline: still list networks */
    if (!w) return;

    N.wifi = 1;
    if (!N.up) snprintf(N.iface, sizeof N.iface, "%s", w->name);
    snprintf(N.ssid, sizeof N.ssid, "%s", w->ssid);
    N.freq = w->freq;
    if (N.up) nl_call(mk_msg(NL80211_CMD_GET_STATION, NLM_F_DUMP, w->idx), h_station, &N.dbm);
}

static int wifi_ifindex(void)
{
    for (int i = 0; i < nwif; i++)
        if (!strcmp(wifs[i].name, N.iface)) return wifs[i].idx;
    return nwif ? wifs[0].idx : 0;
}

/* ------------------------------------------------------------------ */
/* Glyphs                                                              */
/* ------------------------------------------------------------------ */

static void wifi_glyph(cairo_t *cr, double cx, double cy, double s, Rgba c, int lit)
{
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_translate(cr, cx, cy + s * 0.18);
    cairo_scale(cr, s / 20.0, s / 20.0);
    cairo_set_line_width(cr, 1.9);
    for (int i = 0; i < 3; i++) {
        Rgba cc = c;
        if (i >= lit) cc.a *= 0.3;
        ui_color(cr, cc);
        cairo_arc(cr, 0, 6, 5 + i * 5, -2.36, -0.78);
        cairo_stroke(cr);
    }
    ui_color(cr, c);
    cairo_arc(cr, 0, 6, 1.6, 0, 6.2832);
    cairo_fill(cr);
    cairo_restore(cr);
}

static const char *quality_str(int dbm)
{
    return dbm >= -66 ? "Excellent" : dbm >= -70 ? "Good" :
           dbm >= -80 ? "Fair" : "Unusable";
}

static long scan_until;                      /* ms: show "Scanning..." */

static long now_ms(void) { return (long)mono(); }

static void do_scan(void)
{
    if (!nls || !N.wifi) return;
    int idx = wifi_ifindex();
    trigger_scan(idx);
    scan_dump(idx);
    scan_until = now_ms() + 4000;
    tick_n = 0;
}

static int level_from_dbm(int dbm)
{
    return dbm >= -55 ? 3 : dbm >= -67 ? 2 : 1;
}

static int cur_level(void)
{
    return (N.wifi && N.have_dbm) ? level_from_dbm(N.dbm) : 3;
}

/* Ethernet (RJ45) port: socket outline with the plug opening and its latch
 * notch, centred on (cx,cy). */
static void wired_glyph(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 20.0, s / 20.0);
    ui_color(cr, c);
    cairo_set_line_width(cr, 1.9);
    double x0 = -8, y0 = -7, x1 = 8, y1 = 7, r = 2;     /* socket */
    cairo_move_to(cr, x0 + r, y0);
    cairo_arc(cr, x1 - r, y0 + r, r, -1.5708, 0);
    cairo_arc(cr, x1 - r, y1 - r, r, 0, 1.5708);
    cairo_arc(cr, x0 + r, y1 - r, r, 1.5708, 3.14159);
    cairo_arc(cr, x0 + r, y0 + r, r, 3.14159, 4.71239);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, -5, 4);                           /* opening + notch */
    cairo_line_to(cr, -5, -2);
    cairo_line_to(cr, -2.5, -2);
    cairo_line_to(cr, -2.5, -4);
    cairo_line_to(cr, 2.5, -4);
    cairo_line_to(cr, 2.5, -2);
    cairo_line_to(cr, 5, -2);
    cairo_line_to(cr, 5, 4);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* Wired look: the default route (lowest metric, see read_route) goes through
 * a real Ethernet interface (not Wi-Fi, VPN or PPP); offline, when there is
 * no wireless interface at all to fall back on. */
static int show_wired(void)
{
    return N.up ? N.ether && !N.wifi : nwif == 0;
}

/* VPN badge: a small disc with a "V", at the bottom right of a glyph of
 * size s centred on (cx,cy).  Ringed in the background colour so it stays
 * readable over the glyph. */
static void vpn_badge(cairo_t *cr, double cx, double cy, double s)
{
    double r = s * 0.21, bx = cx + s * 0.36, by = cy + s * 0.30;
    cairo_save(cr);
    cairo_new_path(cr);
    ui_color(cr, C_BG);
    cairo_arc(cr, bx, by, r + s * 0.05, 0, 6.2832);
    cairo_fill(cr);
    ui_color(cr, C_YELLOW);
    cairo_arc(cr, bx, by, r, 0, 6.2832);
    cairo_fill(cr);
    ui_color(cr, C_BG);
    cairo_set_line_width(cr, r * 0.32);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, bx - r * 0.45, by - r * 0.42);
    cairo_line_to(cr, bx,            by + r * 0.48);
    cairo_line_to(cr, bx + r * 0.45, by - r * 0.42);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* tray icon and popup header glyph */
static void glyph_net(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    if (show_wired()) wired_glyph(cr, cx, cy, s, c);
    else              wifi_glyph(cr, cx, cy, s, c, cur_level());
    if (N.vpn[0]) vpn_badge(cr, cx, cy, s);
}

/* Encryption indicator: closed lock = WPA*, open lock = WEP, "!" = open network */
static void enc_glyph(cairo_t *cr, double cx, double cy, double s, Rgba c, int enc);

static void lock_glyph(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 14.0, s / 14.0);
    ui_color(cr, c);
    cairo_rectangle(cr, -4.5, -1, 9, 7);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_arc(cr, 0, -1.5, 3, 3.14159, 0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void unlock_glyph(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy + 1);
    cairo_scale(cr, s / 14.0, s / 14.0);
    ui_color(cr, c);
    cairo_rectangle(cr, -4.5, -1, 9, 7);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_move_to(cr, -3, -1.5);                     /* shackle lifted on one side */
    cairo_line_to(cr, -3, -4);
    cairo_arc(cr, 0, -4, 3, 3.14159265, 6.2831853);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void excl_glyph(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 14.0, s / 14.0);
    ui_color(cr, c);
    cairo_set_line_width(cr, 2.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, 0, -5.5);
    cairo_line_to(cr, 0, 1.5);
    cairo_stroke(cr);
    cairo_arc(cr, 0, 5, 1.3, 0, 6.2831853);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void enc_glyph(cairo_t *cr, double cx, double cy, double s, Rgba c, int enc)
{
    if (enc == ENC_OPEN) excl_glyph(cr, cx, cy, s, c);
    else if (enc == ENC_WEP) unlock_glyph(cr, cx, cy, s, c);
    else lock_glyph(cr, cx, cy, s, c);
}
/* ------------------------------------------------------------------ */
/* Popup                                                               */
/* ------------------------------------------------------------------ */

static const char *band_str(int f)
{
    if (!f) return "-";
    return f < 3000 ? "2.4GHz" : f < 5925 ? "5GHz" : "6GHz";
}

static void fmt_rate(char *b, size_t n, double bps)
{
    if (bps >= 1e6) snprintf(b, n, "%.1f MB/s", bps / 1e6);
    else snprintf(b, n, "%.1f KB/s", bps / 1e3);
}

static void fmt_total(char *b, size_t n, double bytes)
{
    if (bytes >= 1e9) snprintf(b, n, "%.2f GB", bytes / 1e9);
    else snprintf(b, n, "%.1f MB", bytes / 1e6);
}

typedef struct { int known_y, known_n, other_label, other_y, other_n, other_total, h; } Lay;

#define STATS_Y (UI_HEADER_H + 22)
#define BAND_Y  (STATS_Y + 4 * 26 + 18)
#define DNS_LBL (BAND_Y + 40)
#define DNS_BTN (DNS_LBL + 18)
#define BTN_H   34
#define KNOWN_H 46
#define SCAN_W  96
#define SCAN_H  24
#define REBIND_W 100

static Lay layout(void)
{
    Lay L;
    int nk = 0, no = 0;
    for (int i = 0; i < N.nap; i++) { if (N.aps[i].known) nk++; else no++; }
    if (N.wifi && N.ssid[0] && !nk) nk = 1;             /* connected, not in scan */
    L.known_n = nk;
    L.other_total = no;
    L.other_n = no < LIST_ROWS ? no : LIST_ROWS;
    int y = DNS_BTN + BTN_H + 24;
    L.known_y = y + 18;
    y = L.known_y + nk * KNOWN_H + (nk ? 10 : 0);
    L.other_label = y + 8;
    L.other_y = L.other_label + 18;
    L.h = L.other_y + L.other_n * UI_ROW_H + 12;
    if (!N.wifi) { L.known_n = L.other_n = L.other_total = 0; L.h = DNS_BTN + BTN_H + 20; }
    return L;
}

static void popup_size(int *w, int *h)
{
    Lay L = layout();
    *w = POP_W;
    *h = L.h;
}

static void stat_row(cairo_t *cr, int x, int col, int row, const char *k, const char *v)
{
    int y = STATS_Y + row * 26;
    ui_text(cr, k, x, y, 11, 0, C_DIM, 0);
    ui_text(cr, v, x + col, y, 11, UI_RIGHT, C_FG, col - 80);
}

/* Draw DNS button i's label and a (?) icon centred together as one group,
 * plus a two-line tooltip above the buttons while hovered.  The tooltip is
 * anchored to the popup edge on the same side as the button. */
static void dns_help(cairo_t *cr, int w, int bw, int i, int hover,
                     const char *l1, const char *l2)
{
    const char *lbl = dns_names[i];
    double lw = ui_text_width(lbl, 12, 0), gw = lw + 7 + 14;
    double bx = UI_PAD + i * (bw + 8), sx = bx + (bw - gw) / 2;
    double hx = sx + lw + 7 + 7, hy = DNS_BTN + BTN_H / 2.0;
    ui_text(cr, lbl, sx, hy, 12, 0,
            N.dns == i ? C_FG : (Rgba){0.62, 0.63, 0.70, 1}, 0);
    Rgba hc = hover ? C_FG : C_DIM;
    ui_color(cr, hc);
    cairo_set_line_width(cr, 1.2);
    cairo_arc(cr, hx, hy, 7, 0, 6.2832);
    cairo_stroke(cr);
    ui_text(cr, "?", hx, hy, 10, UI_CENTER | UI_BOLD, hc, 0);
    if (!hover) return;
    double w1 = ui_text_width(l1, 11, 0), w2 = ui_text_width(l2, 11, 0);
    double tw = (w1 > w2 ? w1 : w2) + 24, th = 46;
    double tx = bx + bw / 2.0 < w / 2.0 ? UI_PAD : w - UI_PAD - tw;
    double ty = DNS_BTN - th - 4;
    ui_fill(cr, tx, ty, tw, th, (Rgba){0.04, 0.045, 0.06, 1});
    ui_frame(cr, tx, ty, tw, th, C_ACCENT);
    ui_text(cr, l1, tx + 12, ty + 15, 11, 0, C_FG, 0);
    ui_text(cr, l2, tx + 12, ty + 32, 11, 0, C_FG, 0);
}

static void popup_draw(cairo_t *cr, int w, int h)
{
    (void)h;
    char b[96], sub[64];
    Lay L = layout();

    if (N.wifi && N.ssid[0] && N.up) {
        if (N.have_dbm) snprintf(sub, sizeof sub, "Signal %d dBm (%s)", N.dbm, quality_str(N.dbm));
        else snprintf(sub, sizeof sub, "Wi-Fi");
    } else if (N.up) snprintf(sub, sizeof sub, "%s", N.wifi ? "Wi-Fi" : N.ether ? "Wired" : "Connected");
    else snprintf(sub, sizeof sub, "Offline");
    if (N.vpn[0]) {                                     /* e.g. "Wired · VPN" */
        size_t l = strlen(sub);
        snprintf(sub + l, sizeof sub - l, " · VPN");
    }
    ui_header(cr, w, glyph_net,
              N.wifi && N.ssid[0] ? N.ssid : N.up ? N.iface : "No network", sub);

    /* stats grid */
    int col = (w - 2 * UI_PAD - 24) / 2;
    int xl = UI_PAD, xr = UI_PAD + col + 24;
    double ms, loss = 0;
    pthread_mutex_lock(&mu);
    ms = ping_ms;
    if (hist_n) {
        int lost = 0;
        for (int i = 0; i < hist_n; i++) lost += !hist[i];
        loss = 100.0 * lost / hist_n;
    }
    pthread_mutex_unlock(&mu);

    if (ms >= 0 && N.up) snprintf(b, sizeof b, "%.0f ms", ms); else snprintf(b, sizeof b, "-");
    stat_row(cr, xl, col, 0, "Ping", b);
    snprintf(b, sizeof b, "%.0f%%", N.up ? loss : 0);
    stat_row(cr, xr, col, 0, "Packet Loss", b);
    fmt_rate(b, sizeof b, N.rx_rate);  stat_row(cr, xl, col, 1, "Receiving", b);
    fmt_rate(b, sizeof b, N.tx_rate);  stat_row(cr, xr, col, 1, "Sending", b);
    fmt_total(b, sizeof b, N.rx_total); stat_row(cr, xl, col, 2, "Downloaded", b);
    fmt_total(b, sizeof b, N.tx_total); stat_row(cr, xr, col, 2, "Uploaded", b);
    stat_row(cr, xl, col, 3, "IP Address", N.ip[0] ? N.ip : "-");
    stat_row(cr, xr, col, 3, "Gateway", N.gw[0] ? N.gw : "-");

    ui_sep(cr, BAND_Y - 14, w);
    if (N.wifi) {
        snprintf(b, sizeof b, "Wi-Fi band: %s", band_str(N.freq));
        ui_text(cr, b, UI_PAD, BAND_Y + 4, 11, UI_CAPS, C_DIM, 0);
        if (N.have_dbm && N.up) {
            snprintf(b, sizeof b, "%d dBm (%s)", N.dbm, quality_str(N.dbm));
            ui_text(cr, b, w - UI_PAD, BAND_Y + 4, 11, UI_RIGHT | UI_CAPS, C_DIM, 0);
        }
    } else {
        ui_text(cr, !N.up ? "No connection" : N.ether ? "Wired connection" : "Connected",
                UI_PAD, BAND_Y + 4, 11, UI_CAPS, C_DIM, 0);
    }
    ui_sep(cr, BAND_Y + 22, w);

    /* DNS */
    ui_section(cr, UI_PAD, DNS_LBL, "DNS provider");
    int bw = (w - 2 * UI_PAD - 3 * 8) / 4;
    for (int i = 0; i < 4; i++)
        ui_button(cr, UI_PAD + i * (bw + 8), DNS_BTN, bw, BTN_H,
                  i == 3 || (i == 0 && N.no_ns) ? "" : dns_names[i], NULL, N.dns == i);

    /* (?) help icon inside the "Custom" button */
    dns_help(cr, w, bw, 3, help_hover,
             "Custom points DNS to localhost (127.0.0.1)",
             "so you can configure a local resolver.");
    /* (?) help icon inside the "DHCP" button when resolv.conf has no nameserver */
    if (N.no_ns)
        dns_help(cr, w, bw, 0, nons_hover,
                 "No DNS servers might be configured if the",
                 "resolv.conf hook is disabled in dhcpcd service.");
    if (!N.wifi) return;
    ui_sep(cr, DNS_BTN + BTN_H + 12, w);

    /* known networks */
    if (L.known_n) {
        ui_section(cr, UI_PAD, L.known_y - 10, "Known networks");
        int row = 0;
        for (int i = 0; i < N.nap; i++) {
            Ap *a = &N.aps[i];
            if (!a->known) continue;
            int y = L.known_y + row++ * KNOWN_H;
            if (a->connected) ui_fill(cr, UI_PAD - 10, y, w - 2 * (UI_PAD - 10), KNOWN_H - 4, C_ROW);
            wifi_glyph(cr, UI_PAD + 8, y + KNOWN_H / 2.0 - 2, 16, C_FG, level_from_dbm(a->dbm));
            ui_text(cr, a->ssid, UI_PAD + 32, y + (a->connected ? 14 : 20), 13, 0, C_FG, w - 130);
            if (a->connected) ui_text(cr, "Connected", UI_PAD + 32, y + 30, 11, 0, C_DIM, 0);
            enc_glyph(cr, w - UI_PAD - 6, y + KNOWN_H / 2.0 - 2, 14, C_DIM, a->enc);
        }
        if (!row) {                                 /* connected but not in scan list */
            int y = L.known_y;
            ui_fill(cr, UI_PAD - 10, y, w - 2 * (UI_PAD - 10), KNOWN_H - 4, C_ROW);
            wifi_glyph(cr, UI_PAD + 8, y + KNOWN_H / 2.0 - 2, 16, C_FG, cur_level());
            ui_text(cr, N.ssid, UI_PAD + 32, y + 14, 13, 0, C_FG, w - 130);
            ui_text(cr, "Connected", UI_PAD + 32, y + 30, 11, 0, C_DIM, 0);
        }
    }

    /* other networks */
    ui_section(cr, UI_PAD, L.other_label, "Other networks");
    ui_button(cr, w - UI_PAD - SCAN_W - 8 - REBIND_W, L.other_label - SCAN_H / 2,
              REBIND_W, SCAN_H, "Rebind IP", NULL, 0);
    ui_button(cr, w - UI_PAD - SCAN_W, L.other_label - SCAN_H / 2, SCAN_W, SCAN_H,
              now_ms() < scan_until ? "Scanning..." : "Scan", NULL, now_ms() < scan_until);
    if (L.other_total) {
        int skip = scroll_off, row = 0;
        for (int i = 0; i < N.nap && row < L.other_n; i++) {
            Ap *a = &N.aps[i];
            if (a->known) continue;
            if (skip-- > 0) continue;
            int y = L.other_y + row * UI_ROW_H;
            if (row == hover_row) ui_fill(cr, UI_PAD - 10, y, w - 2 * (UI_PAD - 10), UI_ROW_H - 2, C_ROW);
            wifi_glyph(cr, UI_PAD + 8, y + UI_ROW_H / 2.0 - 2, 16, C_FG, level_from_dbm(a->dbm));
            ui_text(cr, a->ssid, UI_PAD + 32, y + UI_ROW_H / 2.0 - 1, 13, 0, C_FG, w - 130);
            enc_glyph(cr, w - UI_PAD - 6, y + UI_ROW_H / 2.0 - 2, 14, C_DIM, a->enc);
            row++;
        }
    }
    /* tooltip for the encryption icon under the pointer */
    if (enc_hover >= 0) {
        const char *txt = enc_label[enc_hover];
        double tw = ui_text_width(txt, 11, 0) + 16, th = 22;
        double tx = hover_mx - tw + 12, ty = hover_my - th - 10;
        if (tx < 4) tx = 4;
        if (tx + tw > w - 4) tx = w - 4 - tw;
        if (ty < 4) ty = hover_my + 14;
        ui_fill(cr, tx, ty, tw, th, (Rgba){0.04, 0.045, 0.06, 1});
        ui_frame(cr, tx, ty, tw, th, C_ACCENT);
        ui_text(cr, txt, tx + 8, ty + th / 2, 11, 0, C_FG, 0);
    }
}

static const char *script(void)
{
    const char *s = getenv("SYSTRAY_NET_SCRIPT");
    return s && *s ? s : NET_SCRIPT_DEFAULT;
}

/* script WIFI <interface> <essid> <address> <open|wep|wpa|wpa2|wpa3>
 * (address = AP BSSID)
 * For unknown secured networks the password is passed only in the
 * WIFI_PASSWORD environment variable. */
static char pending_ssid[64], pending_bssid[18];
static int pending_enc;

static void connect_known(const Ap *a)
{
    const char *args[6] = {"WIFI", N.iface, a->ssid, a->bssid, enc_arg[a->enc], NULL};
    app_run_ex(script(), args, NULL, NULL);
}

static void password_done(const char *pw, void *ud)
{
    (void)ud;
    if (!pw) return;                                     /* cancelled */
    const char *args[6] = {"WIFI", N.iface, pending_ssid, pending_bssid,
                           enc_arg[pending_enc], NULL};
    app_run_ex(script(), args, "WIFI_PASSWORD", pw);
}

static void connect_ap(const Ap *a)
{
    if (a->known || a->enc == ENC_OPEN) { connect_known(a); return; }
    char title[96];
    snprintf(title, sizeof title, "Connect to %.60s", a->ssid);
    snprintf(pending_ssid, sizeof pending_ssid, "%.63s", a->ssid);
    snprintf(pending_bssid, sizeof pending_bssid, "%s", a->bssid);
    pending_enc = a->enc;
    app_ask_password(title, a->enc == ENC_WEP ? "WEP key" : "Network password",
                      password_done, NULL);
}
static int other_row_at(Lay *L, int y)
{
    if (y < L->other_y || y >= L->other_y + L->other_n * UI_ROW_H) return -1;
    return (y - L->other_y) / UI_ROW_H;
}

static Ap *other_ap(int row)
{
    int skip = scroll_off;
    for (int i = 0; i < N.nap; i++) {
        if (N.aps[i].known) continue;
        if (skip-- > 0) continue;
        if (row-- == 0) return &N.aps[i];
    }
    return NULL;
}

/* encryption type of the network whose icon is at (x,y), or -1 */
static int enc_at(int x, int y)
{
    if (!N.wifi || x < POP_W - UI_PAD - 14 || x > POP_W - UI_PAD + 6) return -1;
    Lay L = layout();
    int r = other_row_at(&L, y);
    if (r >= 0) {
        Ap *a = other_ap(r);
        return a ? a->enc : -1;
    }
    if (y >= L.known_y && y < L.known_y + L.known_n * KNOWN_H) {
        int k = (y - L.known_y) / KNOWN_H;
        for (int i = 0; i < N.nap; i++)
            if (N.aps[i].known && k-- == 0) return N.aps[i].enc;
    }
    return -1;
}

static void popup_motion(int x, int y)
{
    hover_mx = x;
    hover_my = y;
    enc_hover = enc_at(x, y);
    Lay L = layout();
    hover_row = N.wifi ? other_row_at(&L, y) : -1;
    int bw = (POP_W - 2 * UI_PAD - 3 * 8) / 4;
    help_hover = IN_RECT(x, y, UI_PAD + 3 * (bw + 8), DNS_BTN, bw, BTN_H);
    nons_hover = N.no_ns && IN_RECT(x, y, UI_PAD, DNS_BTN, bw, BTN_H);
}

static void popup_button(int x, int y, int button, int pressed)
{
    if (!pressed) return;
    Lay L = layout();

    if (button == 4 || button == 5) {
        int max = L.other_total - LIST_ROWS;
        if (max < 0) max = 0;
        scroll_off += button == 5 ? 1 : -1;
        if (scroll_off < 0) scroll_off = 0;
        if (scroll_off > max) scroll_off = max;
        return;
    }
    if (button != 1) return;

    if (y >= DNS_BTN && y < DNS_BTN + BTN_H) {
        int bw = (POP_W - 2 * UI_PAD - 3 * 8) / 4;
        for (int i = 0; i < 4; i++)
            if (x >= UI_PAD + i * (bw + 8) && x < UI_PAD + i * (bw + 8) + bw) {
                /* DHCP also gets the interface so the script can renew the lease */
                app_run(script(), "DNS", dns_args[i],
                        i == DNS_DHCP && N.iface[0] ? N.iface : NULL);
                N.dns = i;                           /* optimistic; re-read below */
            }
        return;
    }
    if (!N.wifi) return;

    if (IN_RECT(x, y, POP_W - UI_PAD - SCAN_W, L.other_label - SCAN_H / 2, SCAN_W, SCAN_H)) {
        do_scan();
        return;
    }
    if (IN_RECT(x, y, POP_W - UI_PAD - SCAN_W - 8 - REBIND_W, L.other_label - SCAN_H / 2,
                REBIND_W, SCAN_H)) {
        const char *args[3] = {"REBIND", N.iface, NULL};
        app_run_ex(script(), args, NULL, NULL);
        return;
    }
    int r = other_row_at(&L, y);
    if (r >= 0) {
        Ap *a = other_ap(r);
        if (a) connect_ap(a);
        return;
    }
    if (y >= L.known_y && y < L.known_y + L.known_n * KNOWN_H) {
        int k = (y - L.known_y) / KNOWN_H, row = 0;
        for (int i = 0; i < N.nap; i++) {
            if (!N.aps[i].known) continue;
            if (row++ == k) {
                if (!N.aps[i].connected) connect_known(&N.aps[i]);
                break;
            }
        }
    }
}

static void popup_open(void)
{
    popup_open_flag = 1;
    help_hover = nons_hover = 0;
    enc_hover = -1;
    scroll_off = 0;
    hover_row = -1;
    tick_n = 0;
    load_known();
    do_scan();
}

static void popup_close(void) { popup_open_flag = 0; }

/* ------------------------------------------------------------------ */
/* Icon / module                                                       */
/* ------------------------------------------------------------------ */

static void draw_icon(cairo_t *cr, int w, int h)
{
    Rgba c = N.up ? C_BLUE : C_RED;
    if (show_wired()) wired_glyph(cr, w / 2.0, h / 2.0, h * 0.75, c);
    else              wifi_glyph(cr, w / 2.0, h / 2.0 - 2, h * 0.75, c, cur_level());
    if (N.vpn[0]) vpn_badge(cr, w / 2.0, h / 2.0, h * 0.75);
}

static int tooltip(char *buf, size_t n)
{
    if (!N.up) snprintf(buf, n, "No network");
    else if (N.wifi && N.ssid[0]) snprintf(buf, n, "%s", N.ssid);
    else if (N.ether) snprintf(buf, n, "%s (wired)", N.iface);
    else snprintf(buf, n, "%s", N.iface);
    if (N.up && N.vpn[0] && strcmp(N.vpn, N.iface)) {  /* "MyWifi — VPN wg0" */
        size_t l = strlen(buf);
        if (l < n) snprintf(buf + l, n - l, " — VPN %s", N.vpn);
    }
    return 1;
}

static int tick(void)
{
    read_route();
    wifi_update();
    if (N.up) { read_ip(); read_traffic(); }
    else { N.ip[0] = 0; N.rx_rate = N.tx_rate = 0; }
    N.dns = detect_dns();

    if (popup_open_flag && N.wifi) {
        tick_n++;
        if (tick_n % 10 == 0) trigger_scan(wifi_ifindex());
        if (tick_n % 3 == 0) scan_dump(wifi_ifindex());
    }
    return 1;
}

/* Tools like wpa_cli / dhcpcd live in the sbin dirs, which are usually not in
 * an unprivileged user's PATH.  Append them (for popen() and for scripts). */
static void extend_path(void)
{
    static const char *const dirs[] = {"/sbin", "/usr/sbin", "/usr/local/sbin"};
    const char *old = getenv("PATH");
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", old && *old ? old : "/usr/local/bin:/usr/bin:/bin");
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        size_t n = strlen(dirs[i]);
        int found = 0;
        for (const char *p = buf; (p = strstr(p, dirs[i])); p += n)
            if ((p == buf || p[-1] == ':') && (p[n] == 0 || p[n] == ':')) { found = 1; break; }
        if (!found && strlen(buf) + n + 2 < sizeof buf) {
            strcat(buf, ":");
            strcat(buf, dirs[i]);
        }
    }
    setenv("PATH", buf, 1);
}

static int init(void)
{
    extend_path();
    nls = nl_socket_alloc();
    if (nls && genl_connect(nls) == 0) {
        nl_id = genl_ctrl_resolve(nls, "nl80211");
        if (nl_id < 0) { nl_socket_free(nls); nls = NULL; }
    } else if (nls) {
        nl_socket_free(nls);
        nls = NULL;
    }
    load_known();
    ping_run = 1;
    pthread_create(&ping_th, NULL, ping_thread, NULL);
    tick();
    return 0;
}

static void fini(void)
{
    ping_run = 0;
    pthread_join(ping_th, NULL);
    if (nls) nl_socket_free(nls);
}

const Module network_module = {
    .name = "network", .init = init, .fini = fini, .tick = tick,
    .draw_icon = draw_icon, .tooltip = tooltip,
    .popup_open = popup_open, .popup_close = popup_close,
    .popup_size = popup_size, .popup_draw = popup_draw,
    .popup_button = popup_button, .popup_motion = popup_motion,
};
