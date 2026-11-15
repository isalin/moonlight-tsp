/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_FDGUARD_H
#define MOONLIGHT_TSP_FDGUARD_H

/*
 * Protects the engine's long-lived descriptors (UI socket, timer, wake
 * pipe, stdout/stderr) from stray close() calls elsewhere in the process.
 *
 * The executable exports its own close() (-Wl,--export-dynamic-symbol=close),
 * so every caller goes through it: our code, the static libraries, and the
 * vendor CedarC and ALSA libraries. A close() of a guarded descriptor that
 * does not come from fdguard_close() is a double close somewhere else whose
 * number was reused. It is refused and logged to stderr with the caller's
 * library and offset (for addr2line).
 */

/* Loads the unwinder early; returns 1 if close() is interposed for the
 * shared libraries too, 0 if only for code linked into the executable. */
int fdguard_init(void);

void fdguard_add(int fd, const char *name);

/* The owner's close: unguards FD, then closes it. */
int fdguard_close(int fd);

/* Unguards FD without closing it (it is no longer ours). */
void fdguard_forget(int fd);

/* 0 if FD is guarded but closed or replaced by another file (through a
 * path that bypasses close(): glibc's internal closes, dup2, a raw
 * syscall); 1 otherwise. */
int fdguard_ok(int fd);

/*
 * Checks that each guarded descriptor still refers to the file it was
 * registered with, and logs each one that does not (once). Returns the
 * number of bad descriptors.
 */
int fdguard_check(void);

/* Stray close() calls refused so far. */
unsigned long fdguard_refused(void);

#endif
