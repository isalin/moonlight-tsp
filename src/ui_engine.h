/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_UI_ENGINE_H
#define MOONLIGHT_TSP_UI_ENGINE_H

#include <sys/types.h>

/*
 * One moonlight-tsp-stream child at a time, driven over a socketpair
 * (--ipc-fd 3). Its stdout/stderr go to a log file.
 */
struct engine {
    pid_t pid;          /* 0 when none */
    int fd;             /* our end, non-blocking; -1 when none */
    char buf[2048];
    size_t len;
    int status;         /* waitpid status once reaped */
    unsigned long rx;   /* bytes received */
    int eof;            /* 1 after end of file, -errno after a read error */
};

void engine_init(struct engine *e);

/*
 * Starts PATH with ARGS (NULL-terminated, without argv[0]) plus
 * "--ipc-fd 3". LOG receives stdout/stderr; it is truncated unless APPEND.
 */
int engine_spawn(struct engine *e, const char *path, const char *const *args,
                 const char *log, int append);

/* Next complete line (without newline) or 0 if none is buffered. */
int engine_read_line(struct engine *e, char *line, size_t max);

/* Reads from fd into the buffer; returns -1 on EOF/error. */
int engine_fill(struct engine *e);

void engine_send(struct engine *e, const char *cmd);
void engine_signal(struct engine *e, int sig);

/* Non-blocking reap; returns 1 if the child has exited (status set). */
int engine_reap(struct engine *e);

/* Kills (SIGKILL) and reaps, closing the socket. */
void engine_kill(struct engine *e);

/* Closes our end after the child was reaped. */
void engine_close(struct engine *e);

/*
 * Runs PATH reset-display synchronously (at most TIMEOUT_MS), appending
 * to LOG; returns its exit code or -1.
 */
int engine_run_reset(const char *path, const char *log, int timeout_ms);

/*
 * Writes what each thread of PID is doing to PATH before a watchdog kill:
 * state, wait channel, the syscall it is in (with the descriptors of a
 * poll), its kernel stack, and the words on its user stack that point into
 * code (candidate return addresses as file+offset). Then the open
 * descriptors and the mappings. Returns 0 on success.
 */
int engine_dump(pid_t pid, const char *path);

#endif
