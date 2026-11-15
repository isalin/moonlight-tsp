/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_UI_MDNS_H
#define MOONLIGHT_TSP_UI_MDNS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal one-shot mDNS browse for _nvstream._tcp.local (Sunshine and
 * GeForce Experience hosts). The socket shares port 5353 with the
 * device's avahi-daemon (SO_REUSEADDR; avahi allows other stacks by
 * default), joins 224.0.0.251 and reads the multicast answers. Windows'
 * built-in responder, which Sunshine uses there, ignores "legacy unicast"
 * queries from other ports (RFC 6762 6.7), so those are only the fallback
 * when port 5353 cannot be shared.
 */
#define MDNS_MAX_RESULTS 8

struct mdns_result {
    char name[64];      /* instance name, e.g. the PC's name */
    char addr[16];      /* dotted IPv4 */
    unsigned port;      /* HTTP port from SRV, 0 if absent */
};

struct mdns {
    int fd;
    int legacy;         /* 1: ephemeral port, unicast answers only */
    int n;
    struct mdns_result r[MDNS_MAX_RESULTS];
};

int mdns_open(struct mdns *m);
int mdns_query(struct mdns *m);
/* Reads responses; returns the number of new results. */
int mdns_read(struct mdns *m);
void mdns_close(struct mdns *m);

/*
 * Parses one response packet. SRC_ADDR is the sender (network order).
 * The address is an A record of the SRV target, preferring the sender's
 * own address when a PC lists several (virtual adapters, VPNs), else the
 * sender. Exposed for tests.
 */
int mdns_parse(const uint8_t *pkt, size_t len, uint32_t src_addr,
               struct mdns_result *out);

#endif
