/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wifi_ps.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char *iw_path(void)
{
    static const char *const paths[] = { "/usr/sbin/iw", "/sbin/iw",
                                         "/usr/bin/iw" };
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        if (access(paths[i], X_OK) == 0)
            return paths[i];
    return NULL;
}

/* Runs iw with argv; stdout goes to out (if not NULL). Returns the exit
 * status or -1. */
static int run_iw(char *const argv[], char *out, size_t outlen)
{
    const char *iw = iw_path();
    posix_spawn_file_actions_t fa;
    int pfd[2] = { -1, -1 }, status, rc;
    pid_t pid;
    size_t used = 0;
    ssize_t n;

    if (iw == NULL) {
        errno = ENOENT;
        return -1;
    }
    if (out != NULL && pipe2(pfd, O_CLOEXEC) != 0)
        return -1;
    posix_spawn_file_actions_init(&fa);
    if (out != NULL)
        posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);
    rc = posix_spawn(&pid, iw, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (out != NULL)
        close(pfd[1]);
    if (rc != 0) {
        if (out != NULL)
            close(pfd[0]);
        errno = rc;
        return -1;
    }
    if (out != NULL) {
        while (used + 1 < outlen &&
               (n = read(pfd[0], out + used, outlen - 1 - used)) != 0) {
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            used += (size_t)n;
        }
        out[used] = '\0';
        close(pfd[0]);
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int set_ps(const char *state)
{
    char *argv[] = { "iw", "dev", WIFI_PS_IFACE, "set", "power_save",
                     (char *)state, NULL };

    return run_iw(argv, NULL, 0);
}

int wifi_ps_suspend(void)
{
    char *argv[] = { "iw", "dev", WIFI_PS_IFACE, "get", "power_save", NULL };
    char out[128];
    int fd;

    /* A marker from a killed engine: power save is already ours to restore. */
    if (access(WIFI_PS_MARKER, F_OK) == 0) {
        if (set_ps("off") != 0)
            return -1;
        printf("wifi: power save off (left by an earlier session)\n");
        return 1;
    }
    if (run_iw(argv, out, sizeof(out)) != 0)
        return -1;
    if (strstr(out, "Power save: on") == NULL)
        return 0;
    fd = open(WIFI_PS_MARKER, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    if (set_ps("off") != 0) {
        unlink(WIFI_PS_MARKER);
        return -1;
    }
    printf("wifi: power save off for the stream\n");
    return 1;
}

void wifi_ps_restore(void)
{
    if (access(WIFI_PS_MARKER, F_OK) != 0)
        return;
    if (set_ps("on") == 0) {
        unlink(WIFI_PS_MARKER);
        printf("wifi: power save restored\n");
    } else {
        fprintf(stderr, "wifi: could not restore power save\n");
    }
}
