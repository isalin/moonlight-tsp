/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Checks engine_dump against a child blocked on a mutex and in poll
 * ("make check"). */
#include "ui_engine.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *holder(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&m);
    pause();
    return NULL;
}

static void *poller(void *arg)
{
    struct pollfd p[2];

    p[0].fd = ((int *)arg)[0];
    p[0].events = POLLIN;
    p[1].fd = -1;
    p[1].events = 0;
    poll(p, 2, -1);
    return NULL;
}

int main(void)
{
    char path[] = "/tmp/dump-test-XXXXXX", buf[65536];
    pid_t pid;
    FILE *f;
    size_t n;
    int fd, threads = 0, ok;
    char *l;

    pid = fork();
    if (pid == 0) {
        pthread_t t, t2;
        static int pfd[2];

        if (pipe(pfd) != 0)
            return 1;
        pthread_create(&t, NULL, holder, NULL);
        pthread_create(&t2, NULL, poller, pfd);
        usleep(100000);
        pthread_mutex_lock(&m);     /* blocks for good */
        return 0;
    }
    usleep(400000);
    fd = mkstemp(path);
    if (fd < 0)
        return 1;
    close(fd);
    ok = engine_dump(pid, path) == 0;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    f = fopen(path, "r");
    n = f != NULL ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f != NULL)
        fclose(f);
    unlink(path);
    buf[n] = '\0';
    for (l = buf; (l = strstr(l, "\nthread ")) != NULL; l++)
        threads++;
    if (strncmp(buf, "engine ", 7) != 0)
        ok = 0;
    if (threads < 3 || strstr(buf, "  syscall: ") == NULL ||
        strstr(buf, "\nmaps:\n") == NULL)
        ok = 0;
    /* The poller's set (pipe read end, then -1) and the fd table. */
    if (strstr(buf, "  pollfd: ") == NULL || strstr(buf, " -1/0\n") == NULL ||
        strstr(buf, "\nfds:\n") == NULL || strstr(buf, " -> pipe:[") == NULL)
        ok = 0;
    printf("%s  engine_dump: %d threads, %s code addresses\n",
           ok ? "ok  " : "FAIL", threads,
           strstr(buf, "ret:") || strstr(buf, "pc:") ? "with" : "no");
    if (!ok || getenv("DUMP_TEST_SHOW") != NULL)
        fputs(buf, stdout);
    return !ok;
}
