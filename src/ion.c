/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ion.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sunxi_g2d_abi.h"

int ionbuf_alloc(int ion_fd, struct ionbuf *b, size_t len, unsigned heap_id,
                 int prot)
{
    struct tsp_ion_allocation_data ad;
    struct tsp_ion_fd_data fdd;

    b->handle = 0;
    b->fd = -1;
    b->map = NULL;
    b->size = 0;

    memset(&ad, 0, sizeof(ad));
    ad.len = (len + 4095) & ~(size_t)4095;
    ad.align = 4096;
    ad.heap_id_mask = 1u << heap_id;
    ad.flags = 0; /* uncached */
    if (ioctl(ion_fd, TSP_ION_IOC_ALLOC, &ad) != 0)
        return -1;
    b->handle = ad.handle;
    b->size = ad.len;

    memset(&fdd, 0, sizeof(fdd));
    fdd.handle = b->handle;
    if (ioctl(ion_fd, TSP_ION_IOC_MAP, &fdd) != 0 || fdd.fd < 0)
        goto fail;
    b->fd = fdd.fd;
    if (fcntl(b->fd, F_SETFD, FD_CLOEXEC) != 0)
        goto fail;
    if (prot != 0) {
        b->map = mmap(NULL, b->size, prot, MAP_SHARED, b->fd, 0);
        if (b->map == MAP_FAILED) {
            b->map = NULL;
            goto fail;
        }
    }
    return 0;

fail:
    {
        int saved = errno;

        ionbuf_free(ion_fd, b);
        errno = saved;
    }
    return -1;
}

void ionbuf_free(int ion_fd, struct ionbuf *b)
{
    if (b->map != NULL)
        munmap(b->map, b->size);
    if (b->fd >= 0)
        close(b->fd);
    if (b->handle != 0 && ion_fd >= 0) {
        struct tsp_ion_handle_data hd = { .handle = b->handle };

        ioctl(ion_fd, TSP_ION_IOC_FREE, &hd);
    }
    b->map = NULL;
    b->fd = -1;
    b->handle = 0;
    b->size = 0;
}
