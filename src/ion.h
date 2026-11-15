/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_ION_H
#define MOONLIGHT_TSP_ION_H

#include <stddef.h>

/* One buffer from the legacy (kernel 4.9) ION allocator, exported as a dmabuf. */
struct ionbuf {
    int handle;     /* ION handle, 0 when unset */
    int fd;         /* dmabuf fd (close-on-exec), -1 when unset */
    void *map;      /* CPU mapping, or NULL */
    size_t size;    /* page-aligned length */
};

#define IONBUF_INIT { 0, -1, NULL, 0 }

/*
 * Allocates len bytes (rounded up to pages) from heap_id, uncached, and
 * exports a dmabuf fd. prot is 0 for no CPU mapping, else PROT_READ and/or
 * PROT_WRITE. ION system-heap pages arrive zeroed.
 */
int ionbuf_alloc(int ion_fd, struct ionbuf *b, size_t len, unsigned heap_id,
                 int prot);
void ionbuf_free(int ion_fd, struct ionbuf *b);

#endif
