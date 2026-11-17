/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_engine.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IPC_CHILD_FD 3
#define MAX_ARGS 40

extern char **environ;

void engine_init(struct engine *e)
{
    memset(e, 0, sizeof(*e));
    e->fd = -1;
}

static int spawn(const char *path, const char *const *argv, const char *log,
                 int append, int child_fd, pid_t *pid)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    sigset_t none, dfl;
    int rc, logfd = -1;

    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    sigemptyset(&none);
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGPIPE);
    sigaddset(&dfl, SIGCHLD);
    sigaddset(&dfl, SIGTERM);
    sigaddset(&dfl, SIGINT);
    sigaddset(&dfl, SIGHUP);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &dfl);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK |
                                  POSIX_SPAWN_SETSIGDEF);

    if (log != NULL)
        logfd = open(log, O_WRONLY | O_CREAT | O_CLOEXEC |
                     (append ? O_APPEND : O_TRUNC), 0644);
    if (logfd < 0)
        logfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (logfd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, logfd, 1);
        posix_spawn_file_actions_adddup2(&fa, logfd, 2);
    }
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (child_fd >= 0)
        posix_spawn_file_actions_adddup2(&fa, child_fd, IPC_CHILD_FD);

    rc = posix_spawn(pid, path, &fa, &at, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (logfd >= 0)
        close(logfd);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

int engine_spawn(struct engine *e, const char *path, const char *const *args,
                 const char *log, int append)
{
    const char *argv[MAX_ARGS + 4];
    int sv[2], child, n = 0, i;

    if (e->pid > 0)
        return -1;
    argv[n++] = path;
    argv[n++] = "--ipc-fd";
    argv[n++] = "3";
    for (i = 0; args[i] != NULL && n < MAX_ARGS + 3; i++)
        argv[n++] = args[i];
    argv[n] = NULL;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
        return -1;
    /* Keep the child's end away from fd 3 so the dup2 really happens. */
    child = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
    close(sv[1]);
    if (child < 0) {
        close(sv[0]);
        return -1;
    }
    if (spawn(path, argv, log, append, child, &e->pid) != 0) {
        int err = errno;

        close(sv[0]);
        close(child);
        e->pid = 0;
        errno = err;
        return -1;
    }
    close(child);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    e->fd = sv[0];
    e->len = 0;
    e->status = 0;
    e->rx = 0;
    e->eof = 0;
    return 0;
}

int engine_fill(struct engine *e)
{
    ssize_t n;

    if (e->fd < 0)
        return -1;
    if (e->len >= sizeof(e->buf) - 1)
        e->len = 0;     /* an over-long line: drop it */
    n = read(e->fd, e->buf + e->len, sizeof(e->buf) - 1 - e->len);
    if (n > 0) {
        e->len += (size_t)n;
        e->rx += (unsigned long)n;
        return 0;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    e->eof = n == 0 ? 1 : -errno;
    return -1;
}

int engine_read_line(struct engine *e, char *line, size_t max)
{
    char *nl = memchr(e->buf, '\n', e->len);
    size_t n;

    if (nl == NULL)
        return 0;
    n = (size_t)(nl - e->buf);
    if (n >= max)
        n = max - 1;
    memcpy(line, e->buf, n);
    line[n] = '\0';
    n = (size_t)(nl - e->buf) + 1;
    memmove(e->buf, e->buf + n, e->len - n);
    e->len -= n;
    return 1;
}

void engine_send(struct engine *e, const char *cmd)
{
    char line[128];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);

    if (e->fd >= 0 && n > 0 && n < (int)sizeof(line) &&
        write(e->fd, line, (size_t)n) < 0) {
        /* The engine is gone; the reaper notices. */
    }
}

void engine_signal(struct engine *e, int sig)
{
    if (e->pid > 0)
        kill(e->pid, sig);
}

int engine_reap(struct engine *e)
{
    int st;
    pid_t r;

    if (e->pid <= 0)
        return 1;
    r = waitpid(e->pid, &st, WNOHANG);
    if (r == e->pid || (r < 0 && errno == ECHILD)) {
        e->status = r == e->pid ? st : 0;
        e->pid = 0;
        return 1;
    }
    return 0;
}

void engine_close(struct engine *e)
{
    if (e->fd >= 0)
        close(e->fd);
    e->fd = -1;
    e->len = 0;
}

void engine_kill(struct engine *e)
{
    if (e->pid > 0) {
        int st;

        kill(e->pid, SIGKILL);
        while (waitpid(e->pid, &st, 0) < 0 && errno == EINTR)
            ;
        e->status = st;
        e->pid = 0;
    }
    engine_close(e);
}

int engine_run_reset(const char *path, const char *log, int timeout_ms)
{
    const char *argv[] = { path, "reset-display", NULL };
    struct timespec ts = { 0, 20 * 1000000L };
    pid_t pid;
    int st, waited = 0;

    if (spawn(path, argv, log, 1, -1, &pid) != 0)
        return -1;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);

        if (r == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0 && errno != EINTR)
            return -1;
        if (waited >= timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return -1;
        }
        nanosleep(&ts, NULL);
        waited += 20;
    }
}

int engine_spawn_plain(const char *path, const char *const *argv,
                       const char *log, pid_t *pid)
{
    return spawn(path, argv, log, 1, -1, pid);
}

/* ---- hang report ------------------------------------------------------ */

#define DUMP_MAPS 64
#define DUMP_STACK_BYTES 16384
#define DUMP_RETS 40

struct xmap {
    unsigned long lo, hi, off;
    char name[40];
};

/* Reads a whole (small) proc file; returns its length or -1. */
static int read_all(const char *path, char *buf, size_t max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t used = 0;
    ssize_t n;

    if (fd < 0)
        return -1;
    while (used + 1 < max &&
           (n = read(fd, buf + used, max - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    buf[used] = '\0';
    return (int)used;
}

/* Executable file mappings, used to name code addresses. */
static int load_xmaps(const char *maps, struct xmap *x, int max)
{
    const char *l = maps;
    int n = 0;

    while (*l != '\0' && n < max) {
        unsigned long lo, hi, off;
        char perms[8];
        int path_at = 0;
        const char *nl = strchr(l, '\n');

        if (sscanf(l, "%lx-%lx %7s %lx %*s %*s %n", &lo, &hi, perms, &off,
                   &path_at) == 4 && path_at > 0 && perms[2] == 'x' &&
            l[path_at] == '/') {
            const char *p = l + path_at, *base = p, *s;
            size_t len;

            for (s = p; *s != '\0' && *s != '\n'; s++)
                if (*s == '/')
                    base = s + 1;
            len = (size_t)(s - base);
            if (len >= sizeof(x[n].name))
                len = sizeof(x[n].name) - 1;
            memcpy(x[n].name, base, len);
            x[n].name[len] = '\0';
            x[n].lo = lo;
            x[n].hi = hi;
            x[n].off = off;
            n++;
        }
        if (nl == NULL)
            break;
        l = nl + 1;
    }
    return n;
}

static const struct xmap *find_xmap(const struct xmap *x, int n,
                                    unsigned long a)
{
    int i;

    for (i = 0; i < n; i++)
        if (a >= x[i].lo && a < x[i].hi)
            return &x[i];
    return NULL;
}

/* "name+0xFILEOFFSET", which addr2line takes for these PIE images. */
static void put_code(FILE *f, const struct xmap *m, unsigned long a)
{
    fprintf(f, "%s+0x%lx", m->name, a - m->lo + m->off);
}

static void dump_thread(FILE *f, pid_t pid, const char *tid, int mem,
                        const struct xmap *x, int nx)
{
    static unsigned char stack[DUMP_STACK_BYTES];
    char path[320], buf[4096], *p, *e;
    unsigned long sp = 0, pc = 0, a[6];
    long nr = 0;
    int n, rets = 0;

    snprintf(path, sizeof(path), "/proc/%d/task/%s/stat", (int)pid, tid);
    if (read_all(path, buf, sizeof(buf)) <= 0)
        return;
    p = strchr(buf, '(');
    e = strrchr(buf, ')');
    if (p == NULL || e == NULL || e < p)
        return;
    *e = '\0';
    fprintf(f, "thread %s (%s) state %c", tid, p + 1, e[2]);
    snprintf(path, sizeof(path), "/proc/%d/task/%s/wchan", (int)pid, tid);
    if (read_all(path, buf, sizeof(buf)) > 0)
        fprintf(f, " wchan %s", buf);
    fputc('\n', f);

    snprintf(path, sizeof(path), "/proc/%d/task/%s/syscall", (int)pid, tid);
    if (read_all(path, buf, sizeof(buf)) > 0) {
        buf[strcspn(buf, "\n")] = '\0';
        fprintf(f, "  syscall: %s\n", buf);
        if (sscanf(buf, "%ld %lx %lx %lx %lx %lx %lx %lx %lx", &nr, &a[0],
                   &a[1], &a[2], &a[3], &a[4], &a[5], &sp, &pc) != 9) {
            if (sscanf(buf, "%ld %lx %lx", &nr, &sp, &pc) != 3)
                sp = pc = 0;
            nr = -1;
        }
    }
    /* ppoll (arm64 73): the descriptors it waits on. */
    if (nr == 73 && mem >= 0 && a[1] > 0) {
        struct pollfd pf[16];
        size_t k, cnt = a[1] < 16 ? (size_t)a[1] : 16;
        ssize_t got = pread(mem, pf, cnt * sizeof(pf[0]), (off_t)a[0]);

        if (got == (ssize_t)(cnt * sizeof(pf[0]))) {
            fputs("  pollfd:", f);
            for (k = 0; k < cnt; k++)
                fprintf(f, " %d/%x", pf[k].fd, (unsigned)pf[k].events);
            fputc('\n', f);
        }
    }
    snprintf(path, sizeof(path), "/proc/%d/task/%s/stack", (int)pid, tid);
    if (read_all(path, buf, sizeof(buf)) > 0) {
        char *l = buf, *nl;

        for (n = 0; *l != '\0' && n < 12; n++, l = nl + 1) {
            char *sym = strstr(l, "] ");

            nl = strchr(l, '\n');
            if (nl != NULL)
                *nl = '\0';
            fprintf(f, "  k: %s\n", sym != NULL ? sym + 2 : l);
            if (nl == NULL)
                break;
        }
    }
    if (pc != 0 && find_xmap(x, nx, pc) != NULL) {
        fputs("  pc: ", f);
        put_code(f, find_xmap(x, nx, pc), pc);
        fputc('\n', f);
    }
    /* Words on the user stack that point into code: return addresses. */
    if (mem >= 0 && sp != 0) {
        ssize_t got = pread(mem, stack, sizeof(stack), (off_t)sp);
        size_t i;

        for (i = 0; got > 0 && i + 8 <= (size_t)got && rets < DUMP_RETS;
             i += 8) {
            unsigned long w;
            const struct xmap *m;

            memcpy(&w, stack + i, sizeof(w));
            m = find_xmap(x, nx, w);
            if (m == NULL)
                continue;
            fprintf(f, "%s", rets % 4 == 0 ? "  ret:" : "");
            fputc(' ', f);
            put_code(f, m, w);
            if (++rets % 4 == 0)
                fputc('\n', f);
        }
        if (rets % 4 != 0)
            fputc('\n', f);
    }
}

/* Open descriptors; anonymous ones (timerfd, eventfd) with their fdinfo. */
static void dump_fds(FILE *f, pid_t pid)
{
    char path[320], target[256], info[1024];
    struct dirent *de;
    DIR *d;

    snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
    d = opendir(path);
    if (d == NULL)
        return;
    fputs("fds:\n", f);
    while ((de = readdir(d)) != NULL) {
        ssize_t n;

        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/proc/%d/fd/%s", (int)pid, de->d_name);
        n = readlink(path, target, sizeof(target) - 1);
        target[n > 0 ? n : 0] = '\0';
        fprintf(f, "  %s -> %s", de->d_name, target);
        snprintf(path, sizeof(path), "/proc/%d/fdinfo/%s", (int)pid,
                 de->d_name);
        if (strncmp(target, "anon_inode:", 11) == 0 &&
            read_all(path, info, sizeof(info)) > 0) {
            char *c;

            for (c = info; *c != '\0'; c++)
                if (*c == '\n')
                    *c = c[1] != '\0' ? ';' : '\0';
            fprintf(f, " [%s]", info);
        }
        fputc('\n', f);
    }
    closedir(d);
}

int engine_dump(pid_t pid, const char *path)
{
    static char maps[32768];
    struct xmap x[DUMP_MAPS];
    char p[64];
    struct dirent *de;
    DIR *d;
    FILE *f;
    time_t now = time(NULL);
    int nx = 0, mem;

    if (pid <= 0)
        return -1;
    f = fopen(path, "we");
    if (f == NULL)
        return -1;
    fprintf(f, "engine %d at %s", (int)pid, ctime(&now));
    snprintf(p, sizeof(p), "/proc/%d/maps", (int)pid);
    if (read_all(p, maps, sizeof(maps)) > 0)
        nx = load_xmaps(maps, x, DUMP_MAPS);
    snprintf(p, sizeof(p), "/proc/%d/mem", (int)pid);
    mem = open(p, O_RDONLY | O_CLOEXEC);
    snprintf(p, sizeof(p), "/proc/%d/task", (int)pid);
    d = opendir(p);
    while (d != NULL && (de = readdir(d)) != NULL)
        if (de->d_name[0] != '.')
            dump_thread(f, pid, de->d_name, mem, x, nx);
    if (d != NULL)
        closedir(d);
    if (mem >= 0)
        close(mem);
    dump_fds(f, pid);
    fprintf(f, "maps:\n%s", maps);
    return fclose(f);
}
