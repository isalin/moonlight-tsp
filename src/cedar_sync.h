/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_CEDAR_SYNC_H
#define MOONLIGHT_TSP_CEDAR_SYNC_H

#include <stdint.h>

/*
 * Blocking replacement for libcdc_base's exported SemTimedWait().
 *
 * The vendor version polls sem_trywait() with an exponential usleep()
 * backoff (10 us doubling up to the timeout), so the SBM parser thread
 * notices a newly submitted frame only at its next poll, often several ms
 * later, and wakes hundreds of times per second while idle. The executable
 * exports its own SemTimedWait (linked with
 * -Wl,--export-dynamic-symbol=SemTimedWait), which the dynamic linker binds
 * libsbm.so's and libcdc_base.so's PLT calls to.
 *
 * Every time a vendor thread is about to block, a sequence number advances.
 * For the SBM parser that happens once it has consumed all submitted data,
 * so a decoder thread can sleep until then instead of polling.
 *
 * Set TSP_SEMWAIT=vendor in the environment to forward to the vendor
 * implementation (A/B testing).
 */

/* Current parser-idle sequence number. Take it before submitting. */
unsigned cedar_sync_seq(void);

/*
 * Sleeps until the sequence differs from seq or CLOCK_MONOTONIC reaches
 * deadline_ns. Returns 1 if the sequence changed, 0 on timeout.
 */
int cedar_sync_wait(unsigned seq, uint64_t deadline_ns);

struct cedar_sync_stats {
    unsigned long calls;    /* SemTimedWait calls that reached us */
    unsigned long blocked;  /* calls that had to sleep */
    unsigned long timeouts; /* sleeps that ended without the semaphore */
    int vendor;             /* 1 if forwarding to the vendor version */
};

void cedar_sync_stats(struct cedar_sync_stats *out);

#endif
