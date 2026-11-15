/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Runs the UI's mDNS browse from the command line: tsp-mdns [seconds]. */
#include "ui_mdns.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ll + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    struct mdns m;
    long long t0 = now_ms(), end = t0 + 1000ll * (argc > 1 ? atoi(argv[1]) : 4);
    long long next = t0;
    int rounds = 0, shown = 0;

    if (mdns_open(&m) != 0) {
        perror("mdns_open");
        return 1;
    }
    printf("mode: %s\n", m.legacy ? "legacy unicast (port 5353 not shared)" :
           "multicast on port 5353");
    while (now_ms() < end) {
        struct pollfd p = { m.fd, POLLIN, 0 };
        long long t = now_ms();

        if (rounds < 3 && t >= next) {
            if (mdns_query(&m) != 0)
                perror("mdns_query");
            rounds++;
            next = t + rounds * 1000;
        }
        poll(&p, 1, 100);
        if (p.revents & POLLIN)
            mdns_read(&m);
        for (; shown < m.n; shown++)
            printf("%5lld ms  %-20s %-15s port %u\n", now_ms() - t0,
                   m.r[shown].name, m.r[shown].addr, m.r[shown].port);
    }
    mdns_close(&m);
    printf("%d found\n", m.n);
    return 0;
}
