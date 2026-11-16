/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_status.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define NL_BUF 16384
#define NL_TIMEOUT_MS 200

/* Reads the first line of PATH into BUF without its newline. */
static int read_line(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t got;

    if (fd < 0)
        return -1;
    got = read(fd, buf, n - 1);
    close(fd);
    if (got < 0)
        return -1;
    buf[got] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int read_in(const char *dir, const char *name, char *buf, size_t n)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return read_line(path, buf, n);
}

/* ---- battery ---------------------------------------------------------- */

static void find_battery(struct ui_status *st, const char *power_dir)
{
    DIR *d = opendir(power_dir);
    struct dirent *e;

    st->bat_dir[0] = '\0';
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        char dir[sizeof(st->bat_dir)], type[32], present[8];
        int n;

        if (e->d_name[0] == '.')
            continue;
        n = snprintf(dir, sizeof(dir), "%s/%s", power_dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(dir))
            continue;
        if (read_in(dir, "type", type, sizeof(type)) != 0 ||
            strcmp(type, "Battery") != 0)
            continue;
        if (read_in(dir, "present", present, sizeof(present)) == 0 &&
            strcmp(present, "0") == 0)
            continue;
        memcpy(st->bat_dir, dir, sizeof(dir));
        break;
    }
    closedir(d);
}

static void poll_battery(struct ui_status *st)
{
    char buf[32], *end;
    long pct;

    st->bat_pct = STATUS_NONE;
    st->bat_charging = 0;
    if (st->bat_dir[0] == '\0' ||
        read_in(st->bat_dir, "capacity", buf, sizeof(buf)) != 0)
        return;
    pct = strtol(buf, &end, 10);
    if (end == buf)
        return;
    st->bat_pct = pct < 0 ? 0 : pct > 100 ? 100 : (int)pct;
    if (read_in(st->bat_dir, "status", buf, sizeof(buf)) == 0)
        st->bat_charging = strcmp(buf, "Charging") == 0 ||
                           strcmp(buf, "Full") == 0;
}

/* ---- Wi-Fi ------------------------------------------------------------ */

int status_wifi_level(int dbm, int prev)
{
    static const int thr[STATUS_WIFI_BARS] = { -80, -70, -60 };
    int lvl = 0;

    while (lvl < STATUS_WIFI_BARS && dbm >= thr[lvl])
        lvl++;
    if (prev >= 0 && prev < lvl) {
        /* Up only 2 dB past each threshold above the bars shown now. */
        lvl = prev;
        while (lvl < STATUS_WIFI_BARS && dbm >= thr[lvl] + 2)
            lvl++;
    }
    return lvl;
}

/* An attribute fits between A and END. The step to the next attribute
 * aligns its length, so A can land past END; compare before subtracting. */
static int nla_ok(const struct nlattr *a, const char *end)
{
    size_t left;

    if ((const char *)a >= end)
        return 0;
    left = (size_t)(end - (const char *)a);
    return left >= NLA_HDRLEN && a->nla_len >= NLA_HDRLEN &&
           a->nla_len <= left;
}

#define NLA_FOR(a, start, len)                                          \
    for (const struct nlattr *a = (const struct nlattr *)(start);       \
         nla_ok(a, (const char *)(start) + (len));                      \
         a = (const struct nlattr *)((const char *)a +                  \
                                     NLA_ALIGN(a->nla_len)))
#define NLA_DATA(a) ((const char *)(a) + NLA_HDRLEN)
#define NLA_LEN(a) ((size_t)(a)->nla_len - NLA_HDRLEN)
#define NLA_TYPE(a) ((a)->nla_type & NLA_TYPE_MASK)

static void nl_close(struct ui_status *st)
{
    if (st->nl_fd >= 0)
        close(st->nl_fd);
    st->nl_fd = -1;
}

static int nl_open(struct ui_status *st)
{
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    struct timeval tv = { 0, NL_TIMEOUT_MS * 1000 };

    if (st->nl_fd >= 0)
        return 0;
    st->nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (st->nl_fd < 0)
        return -1;
    if (setsockopt(st->nl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        bind(st->nl_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        nl_close(st);
        return -1;
    }
    return 0;
}

/*
 * Sends a generic-netlink request with one attribute (TYPE, LEN bytes of
 * DATA) and calls ON_MSG for each reply of this request until the end.
 * Returns 0, or -1 on an error or timeout.
 */
static int nl_request(struct ui_status *st, uint16_t family, uint8_t cmd,
                      uint16_t flags, uint16_t type, const void *data,
                      size_t len,
                      void (*on_msg)(const struct genlmsghdr *g,
                                     const char *attrs, size_t alen,
                                     void *arg),
                      void *arg)
{
    struct {
        struct nlmsghdr n;
        struct genlmsghdr g;
        char attrs[64];
    } req;
    struct nlattr *a = (struct nlattr *)req.attrs;
    static char buf[NL_BUF];
    unsigned seq = ++st->nl_seq;
    int done = 0;

    if (len > sizeof(req.attrs) - NLA_HDRLEN)
        return -1;
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_type = family;
    req.n.nlmsg_flags = (uint16_t)(NLM_F_REQUEST | flags);
    req.n.nlmsg_seq = seq;
    req.g.cmd = cmd;
    req.g.version = 1;
    a->nla_type = type;
    a->nla_len = (uint16_t)(NLA_HDRLEN + len);
    memcpy(req.attrs + NLA_HDRLEN, data, len);
    req.n.nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN + NLA_ALIGN(a->nla_len);
    if (send(st->nl_fd, &req, req.n.nlmsg_len, 0) < 0)
        return -1;

    while (!done) {
        ssize_t got = recv(st->nl_fd, buf, sizeof(buf), 0);
        const struct nlmsghdr *n = (const struct nlmsghdr *)buf;
        size_t left;

        if (got <= 0)
            return -1;      /* error or SO_RCVTIMEO */
        left = (size_t)got;
        for (; NLMSG_OK(n, left); n = NLMSG_NEXT(n, left)) {
            if (n->nlmsg_seq != seq)
                continue;   /* late reply to an older request */
            if (n->nlmsg_type == NLMSG_DONE)
                return 0;
            if (n->nlmsg_type == NLMSG_ERROR) {
                const struct nlmsgerr *e = NLMSG_DATA(n);

                return n->nlmsg_len >= NLMSG_LENGTH(sizeof(*e)) &&
                       e->error == 0 ? 0 : -1;
            }
            if (n->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
                continue;
            on_msg(NLMSG_DATA(n), (const char *)NLMSG_DATA(n) + GENL_HDRLEN,
                   n->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN), arg);
            if (!(n->nlmsg_flags & NLM_F_MULTI))
                done = 1;
        }
    }
    return 0;
}

static void on_family(const struct genlmsghdr *g, const char *attrs,
                      size_t alen, void *arg)
{
    NLA_FOR(a, attrs, alen) {
        if (NLA_TYPE(a) == CTRL_ATTR_FAMILY_ID && NLA_LEN(a) >= 2)
            memcpy(arg, NLA_DATA(a), 2);
    }
}

struct station {
    int found;
    int dbm;
};

static void on_station(const struct genlmsghdr *g, const char *attrs,
                       size_t alen, void *arg)
{
    struct station *s = arg;
    int sig = 0, avg = 0;

    if (g->cmd != NL80211_CMD_NEW_STATION || s->found)
        return;
    NLA_FOR(a, attrs, alen) {
        if (NLA_TYPE(a) != NL80211_ATTR_STA_INFO)
            continue;
        NLA_FOR(i, NLA_DATA(a), NLA_LEN(a)) {
            if (NLA_LEN(i) < 1)
                continue;
            if (NLA_TYPE(i) == NL80211_STA_INFO_SIGNAL)
                sig = *(const int8_t *)NLA_DATA(i);
            else if (NLA_TYPE(i) == NL80211_STA_INFO_SIGNAL_AVG)
                avg = *(const int8_t *)NLA_DATA(i);
        }
    }
    s->found = 1;
    s->dbm = avg < 0 ? avg : sig < 0 ? sig : 0;
}

/* 1: a station (the access point) with S->dbm (0: no signal given);
 * 0: no station; -1: netlink failed. */
static int get_station(struct ui_status *st, struct station *s)
{
    uint32_t ifindex = if_nametoindex(st->ifname);
    uint16_t fam = 0;

    memset(s, 0, sizeof(*s));
    if (ifindex == 0 || nl_open(st) != 0)
        return -1;
    if (st->nl_family == 0) {
        if (nl_request(st, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0,
                       CTRL_ATTR_FAMILY_NAME, "nl80211", sizeof("nl80211"),
                       on_family, &fam) != 0 || fam == 0) {
            nl_close(st);
            return -1;
        }
        st->nl_family = fam;
    }
    if (nl_request(st, (uint16_t)st->nl_family, NL80211_CMD_GET_STATION,
                   NLM_F_DUMP, NL80211_ATTR_IFINDEX, &ifindex,
                   sizeof(ifindex), on_station, s) != 0) {
        /* Start again with a new socket: no late replies to sort out. */
        nl_close(st);
        return -1;
    }
    return s->found;
}

static void poll_wifi(struct ui_status *st)
{
    char state[32];
    struct station s;
    int r;

    if (read_in(st->net_dir, "operstate", state, sizeof(state)) != 0) {
        st->wifi_level = STATUS_NONE;
        st->wifi_dbm = 0;
        return;
    }
    if (strcmp(state, "up") != 0 && strcmp(state, "unknown") != 0) {
        st->wifi_level = STATUS_WIFI_DOWN;
        st->wifi_dbm = 0;
        return;
    }
    r = get_station(st, &s);
    if (r < 0) {
        if (!st->nl_noted) {
            snprintf(st->note, sizeof(st->note), "no Wi-Fi signal from "
                     "nl80211 for %s; showing full bars", st->ifname);
            st->nl_noted = 1;
        }
        st->wifi_dbm = 0;
        st->wifi_level = strcmp(state, "up") == 0 ? STATUS_WIFI_BARS :
                                                    STATUS_WIFI_DOWN;
        return;
    }
    if (r == 0) {
        st->wifi_level = STATUS_WIFI_DOWN;
        st->wifi_dbm = 0;
        return;
    }
    st->wifi_dbm = s.dbm;
    st->wifi_level = s.dbm < 0 ?
                     status_wifi_level(s.dbm, st->wifi_level) :
                     STATUS_WIFI_BARS;
}

/* ---- API -------------------------------------------------------------- */

void status_init(struct ui_status *st, const char *power_dir,
                 const char *net_dir, const char *ifname)
{
    memset(st, 0, sizeof(*st));
    st->nl_fd = -1;
    st->bat_pct = STATUS_NONE;
    st->wifi_level = STATUS_NONE;
    snprintf(st->ifname, sizeof(st->ifname), "%s", ifname);
    snprintf(st->net_dir, sizeof(st->net_dir), "%s/%s", net_dir, ifname);
    find_battery(st, power_dir);
}

int status_poll(struct ui_status *st)
{
    int pct = st->bat_pct, chg = st->bat_charging, lvl = st->wifi_level;

    poll_battery(st);
    poll_wifi(st);
    return pct != st->bat_pct || chg != st->bat_charging ||
           lvl != st->wifi_level;
}

void status_close(struct ui_status *st)
{
    nl_close(st);
}
