/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_mdns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVICE "_nvstream._tcp.local"
#define T_A 1
#define T_PTR 12
#define T_SRV 33
#define MDNS_GROUP 0xe00000fbu      /* 224.0.0.251 */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/*
 * Reads the (possibly compressed) name at *off into OUT as dotted text and
 * its first label raw into FIRST (if not NULL). Advances *off past the name
 * in place. Returns 0 or -1 on malformed input.
 */
static int read_name(const uint8_t *pkt, size_t len, size_t *off, char *out,
                     size_t max, char *first, size_t fmax)
{
    size_t p = *off, n = 0;
    int jumps = 0, jumped = 0, labels = 0;

    if (max == 0)
        return -1;
    out[0] = '\0';
    if (first != NULL && fmax > 0)
        first[0] = '\0';
    for (;;) {
        unsigned l;

        if (p >= len)
            return -1;
        l = pkt[p];
        if ((l & 0xc0) == 0xc0) {
            if (p + 1 >= len || ++jumps > 16)
                return -1;
            if (!jumped)
                *off = p + 2;
            jumped = 1;
            p = (size_t)((l & 0x3f) << 8 | pkt[p + 1]);
            continue;
        }
        if (l & 0xc0)
            return -1;
        if (l == 0) {
            if (!jumped)
                *off = p + 1;
            out[n] = '\0';
            return 0;
        }
        if (p + 1 + l > len)
            return -1;
        if (labels == 0 && first != NULL && fmax > 0) {
            size_t c = l < fmax - 1 ? l : fmax - 1;

            memcpy(first, pkt + p + 1, c);
            first[c] = '\0';
        }
        if (n + l + 2 > max)
            return -1;
        if (labels > 0)
            out[n++] = '.';
        memcpy(out + n, pkt + p + 1, l);
        n += l;
        labels++;
        p += 1 + l;
    }
}

/* Replaces bytes that cannot be shown or stored with '?'. */
static void sanitize(char *s)
{
    for (; *s != '\0'; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f)
            *s = '?';
}

#define MAX_RECS 32

int mdns_parse(const uint8_t *pkt, size_t len, uint32_t src_addr,
               struct mdns_result *out)
{
    char name[256], inst[256], target[256], first[64];
    size_t off = 12, srv[MAX_RECS], arec[MAX_RECS];
    unsigned qd, rr, i, nsrv = 0, na = 0;
    int have_ptr = 0, found = 0;
    uint32_t addr = src_addr;
    struct in_addr ia;

    if (len < 12 || !(pkt[2] & 0x80))      /* responses only */
        return 0;
    memset(out, 0, sizeof(*out));
    inst[0] = target[0] = '\0';
    qd = rd16(pkt + 4);
    rr = (unsigned)rd16(pkt + 6) + rd16(pkt + 8) + rd16(pkt + 10);
    for (i = 0; i < qd; i++) {
        if (read_name(pkt, len, &off, name, sizeof(name), NULL, 0) != 0 ||
            off + 4 > len)
            return 0;
        off += 4;
    }
    /* First pass: the instance (PTR) and where the SRV and A records are. */
    for (i = 0; i < rr; i++) {
        size_t rec = off, rd;
        unsigned type, rdlen;

        if (read_name(pkt, len, &off, name, sizeof(name), NULL, 0) != 0 ||
            off + 10 > len)
            return 0;
        type = rd16(pkt + off);
        rdlen = rd16(pkt + off + 8);
        rd = off + 10;
        if (rd + rdlen > len)
            return 0;
        off = rd + rdlen;
        if (type == T_PTR && !have_ptr && strcasecmp(name, SERVICE) == 0) {
            size_t p = rd;

            if (read_name(pkt, len, &p, inst, sizeof(inst), first,
                          sizeof(first)) == 0 && first[0] != '\0') {
                snprintf(out->name, sizeof(out->name), "%s", first);
                have_ptr = 1;
            }
        } else if (type == T_SRV && rdlen >= 7 && nsrv < MAX_RECS) {
            srv[nsrv++] = rec;
        } else if (type == T_A && rdlen == 4 && na < MAX_RECS) {
            arec[na++] = rec;
        }
    }
    if (!have_ptr)
        return 0;
    /* The SRV record of that instance gives the port and the host name. */
    for (i = 0; i < nsrv; i++) {
        size_t p = srv[i], rd;

        if (read_name(pkt, len, &p, name, sizeof(name), NULL, 0) != 0 ||
            strcasecmp(name, inst) != 0)
            continue;
        rd = p + 10;
        out->port = rd16(pkt + rd + 4);
        p = rd + 6;
        if (read_name(pkt, len, &p, target, sizeof(target), NULL, 0) != 0)
            target[0] = '\0';
        break;
    }
    /* An A record of that host; the sender's own address if listed. */
    for (i = 0; i < na; i++) {
        size_t p = arec[i];
        uint32_t a;

        if (read_name(pkt, len, &p, name, sizeof(name), NULL, 0) != 0)
            continue;
        if (target[0] != '\0' && strcasecmp(name, target) != 0)
            continue;
        memcpy(&a, pkt + p + 10, 4);
        if (a == src_addr) {
            addr = a;
            break;
        }
        if (!found) {
            addr = a;
            found = 1;
        }
    }
    sanitize(out->name);
    ia.s_addr = addr;
    if (inet_ntop(AF_INET, &ia, out->addr, sizeof(out->addr)) == NULL)
        return 0;
    return 1;
}

static int open_socket(unsigned port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int one = 1, ttl = 255;

    if (fd < 0)
        return -1;
    if (port != 0)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    /* RFC 6762 11: mDNS packets are sent with TTL 255. */
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    return fd;
}

int mdns_open(struct mdns *m)
{
    struct ip_mreq mr;

    memset(m, 0, sizeof(*m));
    memset(&mr, 0, sizeof(mr));
    mr.imr_multiaddr.s_addr = htonl(MDNS_GROUP);
    mr.imr_interface.s_addr = htonl(INADDR_ANY);
    m->fd = open_socket(5353);
    if (m->fd >= 0 && setsockopt(m->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr,
                                 sizeof(mr)) != 0) {
        close(m->fd);
        m->fd = -1;
    }
    if (m->fd < 0) {
        m->legacy = 1;
        m->fd = open_socket(0);
    }
    return m->fd >= 0 ? 0 : -1;
}

int mdns_query(struct mdns *m)
{
    uint8_t q[] = {
        0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        9, '_', 'n', 'v', 's', 't', 'r', 'e', 'a', 'm',
        4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0, T_PTR, 0, 1,
    };
    struct sockaddr_in to;

    if (m->fd < 0)
        return -1;
    /* Queries from port 5353 use ID 0 (RFC 6762 18.1); a legacy query's
     * ID is echoed in the unicast answer. */
    if (m->legacy) {
        q[0] = 0x4d;
        q[1] = 0x4c;
    }
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(5353);
    to.sin_addr.s_addr = htonl(MDNS_GROUP);
    return sendto(m->fd, q, sizeof(q), 0, (struct sockaddr *)&to,
                  sizeof(to)) == (ssize_t)sizeof(q) ? 0 : -1;
}

int mdns_read(struct mdns *m)
{
    uint8_t pkt[1500];
    int added = 0;

    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        struct mdns_result r;
        ssize_t n = recvfrom(m->fd, pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&from, &fl);
        int i;

        if (n < 0)
            break;
        if (!mdns_parse(pkt, (size_t)n, from.sin_addr.s_addr, &r))
            continue;
        for (i = 0; i < m->n; i++)
            if (strcmp(m->r[i].addr, r.addr) == 0)
                break;
        if (i < m->n || m->n == MDNS_MAX_RESULTS)
            continue;
        m->r[m->n++] = r;
        added++;
    }
    return added;
}

void mdns_close(struct mdns *m)
{
    if (m->fd >= 0)
        close(m->fd);
    m->fd = -1;
}
