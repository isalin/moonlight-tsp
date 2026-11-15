/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stands in for vendor code: a close() from inside a shared library. */
#include <unistd.h>

int fdguard_caller_close(int fd);

/* Not a tail call: a tail-called close() reports this function's caller. */
int fdguard_caller_close(int fd)
{
    int r = close(fd);

    __asm__ volatile("" ::: "memory");
    return r;
}
