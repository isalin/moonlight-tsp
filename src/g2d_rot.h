/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_G2D_ROT_H
#define MOONLIGHT_TSP_G2D_ROT_H

#include <stddef.h>
#include <stdint.h>

#include "ion.h"

#define G2DROT_MAX_BUFS 8

/*
 * Rotates decoded semi-planar 4:2:0 pictures into a small ring of ION
 * buffers with the G2D rotate unit. The source is given by device (IOMMU)
 * addresses, the destination by dmabuf fd, which the display engine can
 * scan out with DISP_LAYER_SET_CONFIG2. The blit ioctl returns after the
 * hardware has finished, so the source picture can be reused at once.
 */
struct g2drot_buf {
    struct ionbuf ion; /* map: read-only CPU mapping for debugging, or NULL */
    uint32_t iova;     /* device address after g2drot_map_iova, else 0 */
};

struct g2drot {
    int g2d_fd;
    int ion_fd;
    unsigned nbufs;
    unsigned next;
    unsigned dst_w;   /* rotated picture width = luma stride (bytes) */
    unsigned dst_h;   /* rotated picture height */
    int cedar_fd;     /* set by g2drot_map_iova, not owned */
    struct g2drot_buf buf[G2DROT_MAX_BUFS];
};

struct g2drot_src {
    uint64_t addr_y;  /* device address of the luma plane */
    uint64_t addr_c;  /* device address of the interleaved chroma */
    unsigned stride;  /* luma bytes per row (= buffer width in pixels) */
    unsigned alloc_h; /* luma rows in the buffer */
    unsigned crop_x, crop_y, crop_w, crop_h;
    int nv21;         /* 1: V first (NV21), 0: U first (NV12) */
};

/*
 * heap_id: TSP_ION_HEAP_SYS_USER or TSP_ION_HEAP_CMA. dst_w x dst_h is the
 * rotated size (720x1280 for a 1280x720 source turned 90 degrees).
 */
int g2drot_open(struct g2drot *r, unsigned nbufs, unsigned dst_w,
                unsigned dst_h, unsigned heap_id, int map_for_cpu);
void g2drot_close(struct g2drot *r);

/*
 * Maps every buffer into the IOMMU once through cedar_fd so that G2D (and
 * a v1 display layer) can use device addresses instead of importing the
 * dmabuf on every frame. See TSP_CEDAR_IOCTL_GET_IOMMU_ADDR for lifetime
 * rules: the mappings vanish when any Cedar fd of the process is closed,
 * including the decoder's, so map again after recreating the decoder.
 */
int g2drot_map_iova(struct g2drot *r, int cedar_fd);
void g2drot_unmap_iova(struct g2drot *r);

/*
 * Rotates src into the next ring buffer and returns its index, or -1.
 * rot_flag is TSP_G2D_ROT_90 or TSP_G2D_ROT_270 (clockwise angle).
 */
int g2drot_rotate(struct g2drot *r, const struct g2drot_src *src,
                  int rot_flag);

#endif
