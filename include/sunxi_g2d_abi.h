/*
 * Minimal userspace ABI for the A133 (sun50iw10, kernel 4.9) G2D and
 * legacy ION drivers, based on the BSP headers include/linux/g2d_driver.h,
 * drivers/char/sunxi_g2d/g2d_bsp_v2.c and
 * drivers/staging/android/uapi/ion.h. Only what moonlight-tsp uses.
 *
 * The TrimUI kernel's g2d_image_enh is 108 bytes, not the 96 of the public
 * BSP header: disassembling g2d_blit_h and g2d_ioctl in the device's boot
 * image showed gamut at 80, fd at 96, use_phy_addr at 100, the destination
 * image at blit offset 112 and a 220-byte copy_from_user for BITBLT_H.
 * g2d_bsp_bitblt does not read the unknown words at 72, 76 and 104.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MOONLIGHT_TSP_SUNXI_G2D_ABI_H
#define MOONLIGHT_TSP_SUNXI_G2D_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

/* ---- G2D (/dev/g2d) ---- */

/* g2d_fmt_enh: semi-planar 4:2:0. Names give a 32-bit word MSB first. */
enum {
    TSP_G2D_FORMAT_YUV420UVC_V1U1V0U0 = 0x28, /* memory U V U V: NV12 */
    TSP_G2D_FORMAT_YUV420UVC_U1V1U0V0 = 0x29, /* memory V U V U: NV21 */
};

/* g2d_blt_flags_h rotation values for G2D_CMD_BITBLT_H. */
enum {
    TSP_G2D_ROT_90 = 0x100,
    TSP_G2D_ROT_180 = 0x200,
    TSP_G2D_ROT_270 = 0x300,
    TSP_G2D_ROT_0 = 0x400,
    TSP_G2D_ROT_H = 0x1000,
    TSP_G2D_ROT_V = 0x2000,
};

#define TSP_G2D_CMD_BITBLT_H 0x55 /* plain number, not an _IOW code */

struct tsp_g2d_rect {
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
};

/* g2d_image_enh */
struct tsp_g2d_image {
    int32_t bbuff;
    uint32_t color;
    int32_t format;
    uint32_t laddr[3];
    uint32_t haddr[3];
    uint32_t width;     /* buffer width in pixels; pitch = width * bytes */
    uint32_t height;
    uint32_t align[3];  /* pitch alignment, 0 = none */
    struct tsp_g2d_rect clip_rect;
    uint32_t unknown72[2]; /* not in the public header; keep 0 */
    uint32_t gamut;
    int32_t bpremul;
    uint8_t alpha;
    int32_t mode;
    int32_t fd;         /* dmabuf, used when use_phy_addr == 0 */
    uint32_t use_phy_addr; /* 1: laddr/haddr are device (IOMMU) addresses */
    uint32_t unknown104;   /* not in the public header; keep 0 */
};

/* g2d_blt_h */
struct tsp_g2d_blt_h {
    int32_t flag_h;
    struct tsp_g2d_image src_image_h;
    struct tsp_g2d_image dst_image_h;
};

_Static_assert(offsetof(struct tsp_g2d_image, laddr) == 12, "laddr");
_Static_assert(offsetof(struct tsp_g2d_image, width) == 36, "width");
_Static_assert(offsetof(struct tsp_g2d_image, clip_rect) == 56, "clip");
_Static_assert(offsetof(struct tsp_g2d_image, gamut) == 80, "gamut");
_Static_assert(offsetof(struct tsp_g2d_image, alpha) == 88, "alpha");
_Static_assert(offsetof(struct tsp_g2d_image, fd) == 96, "fd");
_Static_assert(offsetof(struct tsp_g2d_image, use_phy_addr) == 100, "phy");
_Static_assert(sizeof(struct tsp_g2d_image) == 108, "g2d_image_enh");
_Static_assert(offsetof(struct tsp_g2d_blt_h, dst_image_h) == 112, "dst");
_Static_assert(sizeof(struct tsp_g2d_blt_h) == 220, "g2d_blt_h");

/* ---- legacy ION (/dev/ion, kernel 4.9) ---- */

/* Heap ids from the device tree (/proc/device-tree/ion/heap_*). */
enum {
    TSP_ION_HEAP_SYS_USER = 0, /* ion_system: pages, IOMMU-mapped */
    TSP_ION_HEAP_CMA = 4,      /* ion_cma: physically contiguous */
};

struct tsp_ion_allocation_data {
    size_t len;
    size_t align;
    unsigned int heap_id_mask;
    unsigned int flags;
    int handle;
};

struct tsp_ion_fd_data {
    int handle;
    int fd;
};

struct tsp_ion_handle_data {
    int handle;
};

#define TSP_ION_IOC_ALLOC _IOWR('I', 0, struct tsp_ion_allocation_data)
#define TSP_ION_IOC_FREE _IOWR('I', 1, struct tsp_ion_handle_data)
#define TSP_ION_IOC_MAP _IOWR('I', 2, struct tsp_ion_fd_data)

_Static_assert(sizeof(struct tsp_ion_allocation_data) == 32, "ion alloc");

/* ---- Cedar IOMMU mapping (/dev/cedar_dev) ---- */

/*
 * IOCTL_GET_IOMMU_ADDR maps a dmabuf into the global IOMMU domain and
 * returns its device address; IOCTL_FREE_IOMMU_ADDR undoes it. The driver
 * records the caller's tgid, and cedardev_release() frees every mapping of
 * that tgid when any /dev/cedar_dev file of the process is released. So
 * closing one's own fd also unmaps the decoder's buffers, and destroying
 * the decoder (libVE closing its fd) also unmaps ours. Verified by
 * disassembling compat_cedardev_ioctl and cedardev_release on the device.
 */
struct tsp_cedar_iommu_param {
    int32_t fd;
    uint32_t iommu_addr;
};

#define TSP_CEDAR_IOCTL_GET_IOMMU_ADDR 0x502 /* plain number */
#define TSP_CEDAR_IOCTL_FREE_IOMMU_ADDR 0x503

#endif
