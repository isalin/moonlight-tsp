/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Checks the close() guard ("make check"). */
#include "fdguard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int fdguard_caller_close(int fd);

static int failures;

static void expect(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        failures++;
}

static int is_open(int fd)
{
    return fcntl(fd, F_GETFD) >= 0;
}

int main(void)
{
    char path[] = "/tmp/fdguard-test-XXXXXX", log[4096];
    int a[2], b[2], logfd, saved, interposed;
    ssize_t n;

    logfd = mkstemp(path);
    if (logfd < 0 || pipe(a) != 0 || pipe(b) != 0)
        return 1;
    unlink(path);
    saved = dup(STDERR_FILENO);
    dup2(logfd, STDERR_FILENO);

    interposed = fdguard_init();
    fdguard_add(a[0], "test pipe");
    expect(close(a[0]) == 0 && is_open(a[0]), "own stray close refused");
    expect(fdguard_caller_close(a[0]) == 0 && is_open(a[0]),
           "stray close from a shared library refused");
    expect(fdguard_check() == 0, "intact fd passes the check");
    expect(fdguard_ok(a[0]) && fdguard_ok(b[1]), "intact and unguarded fds ok");
    dup2(b[0], a[0]);
    expect(fdguard_check() == 1, "replaced fd found by the check");
    expect(!fdguard_ok(a[0]), "replaced fd not ok");
    expect(fdguard_close(a[0]) == 0 && !is_open(a[0]),
           "owner close closes");
    expect(close(a[1]) == 0 && !is_open(a[1]), "unguarded close passes");
    expect(close(a[1]) == -1 && errno == EBADF, "EBADF passes through");
    expect(fdguard_refused() == 2, "refusals counted");
    fdguard_add(b[1], "forgotten");
    fdguard_forget(b[1]);
    expect(close(b[1]) == 0 && !is_open(b[1]), "forgotten fd closes");

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    n = pread(logfd, log, sizeof(log) - 1, 0);
    log[n > 0 ? n : 0] = '\0';
    expect(interposed, "close() exported to shared libraries");
    expect(strstr(log, "of test pipe from fdguard-test+0x") != NULL,
           "caller in the executable named");
    expect(strstr(log, "from libfdguard-caller.so+0x") != NULL,
           "caller in a library named");
    expect(strstr(log, "(test pipe) was replaced: now pipe:[") != NULL,
           "replacement named");
    if (failures || getenv("FDGUARD_TEST_SHOW") != NULL)
        fputs(log, stdout);
    return failures != 0;
}
