/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* sem_clockwait */
#endif
#include "cedar_sync.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef int (*semtimedwait_fn)(sem_t *sem, int64_t time_ms);

static _Atomic unsigned idle_seq;
static _Atomic unsigned waiters;
static _Atomic unsigned long n_calls, n_blocked, n_timeouts;

static pthread_once_t mode_once = PTHREAD_ONCE_INIT;
static semtimedwait_fn vendor_fn;

static void pick_mode(void)
{
    const char *mode = getenv("TSP_SEMWAIT");
    void *h;

    if (mode == NULL || strcmp(mode, "vendor") != 0)
        return;
    h = dlopen("libcdc_base.so", RTLD_NOW | RTLD_NOLOAD);
    if (h == NULL)
        return;
    /* A handle lookup starts at libcdc_base itself, not at us. */
    vendor_fn = (semtimedwait_fn)dlsym(h, "SemTimedWait");
    dlclose(h);
}

static void announce_block(void)
{
    /* Sequentially consistent: pairs with the waiter's waiters++/load. */
    atomic_fetch_add(&idle_seq, 1);
    if (atomic_load(&waiters) != 0)
        syscall(SYS_futex, &idle_seq, FUTEX_WAKE_PRIVATE, INT_MAX, NULL,
                NULL, 0);
}

/*
 * Same contract as the vendor function: wait up to time_ms milliseconds
 * (-1 = forever) and return 0 if the semaphore was taken, -1 otherwise.
 */
__attribute__((visibility("default"))) int SemTimedWait(sem_t *sem,
                                                         int64_t time_ms)
{
    struct timespec ts;
    int r;

    pthread_once(&mode_once, pick_mode);
    if (vendor_fn != NULL)
        return vendor_fn(sem, time_ms);

    atomic_fetch_add_explicit(&n_calls, 1, memory_order_relaxed);
    if (sem_trywait(sem) == 0)
        return 0;
    if (errno != EAGAIN)
        return -1;
    if (time_ms < -1 || time_ms == 0)
        return -1;

    atomic_fetch_add_explicit(&n_blocked, 1, memory_order_relaxed);
    announce_block();

    if (time_ms == -1) {
        do
            r = sem_wait(sem);
        while (r != 0 && errno == EINTR);
        return r == 0 ? 0 : -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)(time_ms / 1000);
    ts.tv_nsec += (long)(time_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    do
        r = sem_clockwait(sem, CLOCK_MONOTONIC, &ts);
    while (r != 0 && errno == EINTR);
    if (r != 0) {
        atomic_fetch_add_explicit(&n_timeouts, 1, memory_order_relaxed);
        return -1;
    }
    return 0;
}

unsigned cedar_sync_seq(void)
{
    return atomic_load(&idle_seq);
}

int cedar_sync_wait(unsigned seq, uint64_t deadline_ns)
{
    int changed = 0;

    atomic_fetch_add(&waiters, 1);
    for (;;) {
        struct timespec now, rel;
        uint64_t now_ns;

        if (atomic_load(&idle_seq) != seq) {
            changed = 1;
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
        if (now_ns >= deadline_ns)
            break;
        rel.tv_sec = (time_t)((deadline_ns - now_ns) / 1000000000ull);
        rel.tv_nsec = (long)((deadline_ns - now_ns) % 1000000000ull);
        /* Returns at once if idle_seq already moved past seq. */
        syscall(SYS_futex, &idle_seq, FUTEX_WAIT_PRIVATE, seq, &rel, NULL, 0);
    }
    atomic_fetch_sub(&waiters, 1);
    return changed;
}

void cedar_sync_stats(struct cedar_sync_stats *out)
{
    pthread_once(&mode_once, pick_mode);
    out->calls = atomic_load(&n_calls);
    out->blocked = atomic_load(&n_blocked);
    out->timeouts = atomic_load(&n_timeouts);
    out->vendor = vendor_fn != NULL;
}
