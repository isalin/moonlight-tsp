/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for mdns_parse (run by "make check"). */
#include "ui_mdns.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static uint8_t pkt[1500];
static size_t len;
static int failures;

static void put16(unsigned v)
{
    pkt[len++] = (uint8_t)(v >> 8);
    pkt[len++] = (uint8_t)v;
}

static void put_name(const char *dotted)
{
    while (*dotted != '\0') {
        const char *dot = strchr(dotted, '.');
        size_t l = dot ? (size_t)(dot - dotted) : strlen(dotted);

        pkt[len++] = (uint8_t)l;
        memcpy(pkt + len, dotted, l);
        len += l;
        dotted += l + (dot ? 1 : 0);
    }
    pkt[len++] = 0;
}

static void begin(int response, unsigned an, unsigned ar)
{
    len = 0;
    put16(0);
    put16(response ? 0x8400 : 0);
    put16(0);
    put16(an);
    put16(0);
    put16(ar);
}

static void rr_head(const char *name, unsigned type, unsigned rdlen)
{
    put_name(name);
    put16(type);
    put16(0x8001);
    put16(0);
    put16(120);
    put16(rdlen);
}

static void ptr(const char *inst)
{
    size_t at;

    rr_head("_nvstream._tcp.local", 12, 0);
    at = len - 2;
    put_name(inst);
    pkt[at] = (uint8_t)((len - at - 2) >> 8);
    pkt[at + 1] = (uint8_t)(len - at - 2);
}

static void srv(const char *inst, unsigned port, const char *target)
{
    size_t at;

    rr_head(inst, 33, 0);
    at = len - 2;
    put16(0);
    put16(0);
    put16(port);
    put_name(target);
    pkt[at] = (uint8_t)((len - at - 2) >> 8);
    pkt[at + 1] = (uint8_t)(len - at - 2);
}

static void a(const char *name, const char *addr)
{
    rr_head(name, 1, 4);
    inet_pton(AF_INET, addr, pkt + len);
    len += 4;
}

static void aaaa(const char *name)
{
    rr_head(name, 28, 16);
    memset(pkt + len, 0xfe, 16);
    len += 16;
}

static void expect(const char *what, const char *src, int want,
                   const char *name, const char *addr, unsigned port)
{
    struct mdns_result r;
    struct in_addr s;
    int got;

    inet_pton(AF_INET, src, &s);
    got = mdns_parse(pkt, len, s.s_addr, &r);
    if (got != want || (want && (strcmp(r.name, name) != 0 ||
                                 strcmp(r.addr, addr) != 0 ||
                                 r.port != port))) {
        printf("FAIL %s: got %d \"%s\" %s %u\n", what, got,
               got ? r.name : "", got ? r.addr : "", got ? r.port : 0);
        failures++;
    } else {
        printf("ok   %s\n", what);
    }
}

int main(void)
{
    size_t i, full;

    /* A Windows PC with a virtual adapter listed first. */
    begin(1, 1, 3);
    ptr("gaming-pc._nvstream._tcp.local");
    srv("gaming-pc._nvstream._tcp.local", 47989, "gaming-pc.local");
    a("gaming-pc.local", "172.20.0.1");
    a("gaming-pc.local", "192.168.0.20");
    expect("prefers the sender's own address", "192.168.0.20", 1, "gaming-pc",
           "192.168.0.20", 47989);
    expect("else the first A of the host", "192.168.0.5", 1, "gaming-pc",
           "172.20.0.1", 47989);

    /* macOS answer: PTR, SRV, AAAA, A. */
    begin(1, 1, 3);
    ptr("mac._nvstream._tcp.local");
    srv("mac._nvstream._tcp.local", 47989, "mac.local");
    aaaa("mac.local");
    a("mac.local", "192.168.0.30");
    expect("A after AAAA", "192.168.0.30", 1, "mac", "192.168.0.30",
           47989);

    /* A records of other hosts are not used. */
    begin(1, 1, 2);
    ptr("htpc._nvstream._tcp.local");
    srv("htpc._nvstream._tcp.local", 47989, "htpc.local");
    a("other.local", "10.9.9.9");
    expect("A of another host ignored", "192.168.0.40", 1, "htpc",
           "192.168.0.40", 47989);

    /* PTR only: the sender's address, no port. */
    begin(1, 1, 0);
    ptr("htpc._nvstream._tcp.local");
    expect("PTR only", "192.168.0.40", 1, "htpc", "192.168.0.40", 0);

    /* Records in another order (SRV before PTR). */
    begin(1, 2, 1);
    srv("htpc._nvstream._tcp.local", 48000, "htpc.local");
    ptr("htpc._nvstream._tcp.local");
    a("htpc.local", "192.168.0.40");
    expect("SRV before PTR", "192.168.0.40", 1, "htpc", "192.168.0.40",
           48000);

    /* Not ours: a query, and another service. */
    begin(0, 1, 0);
    ptr("htpc._nvstream._tcp.local");
    expect("query ignored", "192.168.0.40", 0, "", "", 0);
    begin(1, 1, 0);
    rr_head("_airplay._tcp.local", 12, 9);
    put_name("tv.local");
    expect("other service ignored", "192.168.0.50", 0, "", "", 0);

    /* Every truncation of a valid answer is rejected or parsed safely. */
    begin(1, 1, 2);
    ptr("gaming-pc._nvstream._tcp.local");
    srv("gaming-pc._nvstream._tcp.local", 47989, "gaming-pc.local");
    a("gaming-pc.local", "192.168.0.20");
    full = len;
    for (i = 0; i < full; i++) {
        struct mdns_result r;

        len = i;
        (void)mdns_parse(pkt, len, 0, &r);
    }
    printf("ok   truncations\n");

    /* A compression loop. */
    begin(1, 1, 0);
    put16(0xc000 | 12);
    expect("compression loop", "192.168.0.1", 0, "", "", 0);

    if (failures)
        printf("%d failed\n", failures);
    return failures != 0;
}
