/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_DISP_H
#define MOONLIGHT_TSP_DISP_H

#include <stdint.h>

#include "ion.h"
#include "sunxi_disp_abi.h"

/* One display-engine layer owned by this process. */
struct dispv {
    int fd;
    unsigned screen;
    unsigned channel;
    unsigned layer;
    unsigned zorder;
    int enabled;
    unsigned screen_w;
    unsigned screen_h;
};

struct dispv_frame {
    int dmabuf_fd;       /* >= 0: v2 config with dmabuf; < 0: use addr_* */
    uint64_t addr_y;     /* device (IOMMU) address of the luma plane */
    uint64_t addr_c;     /* device address of the interleaved chroma */
    int format;          /* TSP_DISP_FORMAT_YUV420_SP_* */
    int color_space;     /* TSP_DISP_BT601 etc. */
    unsigned stride;     /* luma bytes per row */
    unsigned alloc_h;    /* luma rows before the chroma plane */
    unsigned crop_x, crop_y, crop_w, crop_h;
    struct tsp_disp_rect win;
};

int dispv_open(struct dispv *d, unsigned channel, unsigned layer,
               unsigned zorder);
int dispv_show(struct dispv *d, const struct dispv_frame *f);
int dispv_hide(struct dispv *d);

/*
 * Every SET_CONFIG2 makes the driver import the dmabuf, and it keeps each
 * import (and so the buffer) until a later SET_CONFIG2 commit finds it
 * older than the newest DISPV_IMPORT_KEEP vsync ids. Disabling the layer
 * does not release them, so the last frames would stay allocated after the
 * process exits. This shows dummy_fd, a black DISPV_DUMMY_SIZE square NV12
 * picture, at zero global alpha for DISPV_IMPORT_KEEP + 2 vsyncs, which
 * releases every older import, then disables the layer. Only the dummy
 * stays referenced, until the next session's commits. Takes about 180 ms.
 */
#define DISPV_IMPORT_KEEP 7
#define DISPV_DUMMY_SIZE 16
int dispv_flush_imports(struct dispv *d, int dummy_fd);

/* Allocates the black dummy picture for dispv_flush_imports. */
int dispv_alloc_dummy(int ion_fd, struct ionbuf *b);

/*
 * Recovery after a crash: disables the layer and, if flush is set, first
 * releases the imports a dead owner left in the driver.
 */
int dispv_reset(unsigned channel, unsigned layer, unsigned zorder, int flush);
void dispv_close(struct dispv *d);

/* Reads one layer configuration (read-only). */
int dispv_get_config(int fd, unsigned screen, unsigned channel,
                     unsigned layer, struct tsp_disp_layer_config2 *out);

/* Disables a layer without needing an open dispv. */
int dispv_disable_layer(int fd, unsigned screen, unsigned channel,
                        unsigned layer);

/* Fits a w x h picture inside the screen, preserving aspect ratio. */
struct tsp_disp_rect dispv_fit(unsigned screen_w, unsigned screen_h,
                               unsigned w, unsigned h);

#endif
