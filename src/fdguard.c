/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fdguard.h"

#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GUARD_MAX 8
#define REPORT_MAX 8
#define BT_MAX 10

struct guard {
    _Atomic int fd1;        /* fd + 1; 0 for a free slot */
    const char *name;
    dev_t dev;
    ino_t ino;
    int bad_logged;         /* main thread only */
};

static struct guard G[GUARD_MAX];
static _Atomic unsigned long n_refused;

/* glibc's real close (close itself is a weak alias of it). */
extern int __close(int fd);

/* "lib+0xOFF (sym+0xOFF)" for a code address. */
static int describe(char *out, size_t max, const void *a)
{
    Dl_info di;
    const char *base;

    if (dladdr(a, &di) == 0 || di.dli_fname == NULL)
        return snprintf(out, max, "%p", a);
    base = strrchr(di.dli_fname, '/');
    base = base != NULL ? base + 1 : di.dli_fname;
    if (di.dli_sname != NULL && di.dli_saddr != NULL)
        return snprintf(out, max, "%s+0x%lx (%s+0x%lx)", base,
                        (unsigned long)((uintptr_t)a -
                                        (uintptr_t)di.dli_fbase),
                        di.dli_sname,
                        (unsigned long)((uintptr_t)a -
                                        (uintptr_t)di.dli_saddr));
    return snprintf(out, max, "%s+0x%lx", base,
                    (unsigned long)((uintptr_t)a - (uintptr_t)di.dli_fbase));
}

static void put(char *line, size_t max, size_t *len, int n)
{
    if (n > 0)
        *len += (size_t)n;
    if (*len >= max)
        *len = max - 1;
}

static __attribute__((noinline)) void refuse(int fd, const char *name,
                                             const void *caller)
{
    char line[1024];
    void *bt[BT_MAX];
    size_t len = 0;
    int n, i;

    if (atomic_fetch_add(&n_refused, 1) >= REPORT_MAX)
        return;
    put(line, sizeof(line), &len,
        snprintf(line, sizeof(line), "fdguard: refused close(%d) of %s "
                                     "from ", fd, name));
    put(line, sizeof(line), &len,
        describe(line + len, sizeof(line) - len, caller));
    /* Frames 0 and 1 are refuse() and close(). */
    n = backtrace(bt, BT_MAX);
    for (i = 2; i < n; i++) {
        put(line, sizeof(line), &len,
            snprintf(line + len, sizeof(line) - len, i == 2 ? "\n  bt: "
                                                            : " < "));
        put(line, sizeof(line), &len,
            describe(line + len, sizeof(line) - len, bt[i]));
    }
    put(line, sizeof(line), &len,
        snprintf(line + len, sizeof(line) - len, "\n"));
    if (write(STDERR_FILENO, line, len) < 0) {
        /* nowhere else to report it */
    }
}

int close(int fd)
{
    int i;

    if (fd >= 0)
        for (i = 0; i < GUARD_MAX; i++)
            if (atomic_load_explicit(&G[i].fd1, memory_order_acquire) ==
                fd + 1) {
                refuse(fd, G[i].name, __builtin_return_address(0));
                return 0;   /* what the caller's own close would return */
            }
    return __close(fd);
}

int fdguard_init(void)
{
    void *bt[2];

    /* The first backtrace() dlopens libgcc_s; do it now, not inside a
     * close() called with some library's locks held. */
    (void)backtrace(bt, 2);
    return dlsym(RTLD_DEFAULT, "close") == (void *)close;
}

void fdguard_add(int fd, const char *name)
{
    struct stat st;
    int i;

    if (fd < 0 || fstat(fd, &st) != 0)
        return;
    for (i = 0; i < GUARD_MAX; i++) {
        if (atomic_load(&G[i].fd1) != 0)
            continue;
        G[i].name = name;
        G[i].dev = st.st_dev;
        G[i].ino = st.st_ino;
        G[i].bad_logged = 0;
        atomic_store_explicit(&G[i].fd1, fd + 1, memory_order_release);
        return;
    }
}

int fdguard_close(int fd)
{
    fdguard_forget(fd);
    return __close(fd);
}

void fdguard_forget(int fd)
{
    int i;

    for (i = 0; i < GUARD_MAX; i++)
        if (atomic_load(&G[i].fd1) == fd + 1)
            atomic_store(&G[i].fd1, 0);
}

int fdguard_ok(int fd)
{
    struct stat st;
    int i;

    for (i = 0; i < GUARD_MAX; i++)
        if (atomic_load(&G[i].fd1) == fd + 1)
            return fstat(fd, &st) == 0 && st.st_dev == G[i].dev &&
                   st.st_ino == G[i].ino;
    return 1;
}

int fdguard_check(void)
{
    int i, bad = 0;

    for (i = 0; i < GUARD_MAX; i++) {
        int fd1 = atomic_load(&G[i].fd1);
        struct stat st;
        char path[32], target[128];
        ssize_t n;

        if (fd1 == 0)
            continue;
        if (fstat(fd1 - 1, &st) == 0 && st.st_dev == G[i].dev &&
            st.st_ino == G[i].ino)
            continue;
        bad++;
        if (G[i].bad_logged)
            continue;
        G[i].bad_logged = 1;
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd1 - 1);
        n = readlink(path, target, sizeof(target) - 1);
        target[n > 0 ? n : 0] = '\0';
        fprintf(stderr, "fdguard: fd %d (%s) was replaced: now %s\n",
                fd1 - 1, G[i].name, n > 0 ? target : strerror(errno));
    }
    return bad;
}

unsigned long fdguard_refused(void)
{
    return atomic_load(&n_refused);
}
