/*
 * Minimal user-space view of the Allwinner disp2 ioctl ABI used by the
 * TrimUI Smart Pro's Linux 4.9 kernel (/dev/disp). Only the layer
 * configuration interface is declared. Every disp ioctl takes a pointer
 * to four unsigned longs: {screen, argument pointer, count, extra}.
 *
 * The layout is checked against the running kernel by tsp-probe, which
 * reads back the framebuffer layer through DISP_LAYER_GET_CONFIG2.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MOONLIGHT_TSP_SUNXI_DISP_ABI_H
#define MOONLIGHT_TSP_SUNXI_DISP_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TSP_DISP_GET_SCN_WIDTH = 0x07,
    TSP_DISP_GET_SCN_HEIGHT = 0x08,
    TSP_DISP_LAYER_SET_CONFIG = 0x47,
    TSP_DISP_LAYER_GET_CONFIG = 0x48,
    TSP_DISP_LAYER_SET_CONFIG2 = 0x49,
    TSP_DISP_LAYER_GET_CONFIG2 = 0x4a,
};

enum {
    TSP_DISP_FORMAT_ARGB_8888 = 0x00,
    TSP_DISP_FORMAT_ABGR_8888 = 0x01,
    TSP_DISP_FORMAT_YUV420_SP_UVUV = 0x4c, /* NV12 */
    TSP_DISP_FORMAT_YUV420_SP_VUVU = 0x4d, /* NV21 */
};

enum {
    TSP_DISP_BT709 = 0x101,
    TSP_DISP_BT601 = 0x104,
    TSP_DISP_BT709_F = 0x201,
    TSP_DISP_BT601_F = 0x204,
};

enum {
    TSP_DISP_EOTF_GAMMA22 = 0x004,
};

struct tsp_disp_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

struct tsp_disp_rectsz {
    uint32_t width;
    uint32_t height;
};

struct tsp_disp_rect64 {
    int64_t x;
    int64_t y;
    int64_t width;
    int64_t height;
};

struct tsp_disp_fb_info2 {
    int32_t fd;
    struct tsp_disp_rectsz size[3];
    uint32_t align[3];
    int32_t format;
    int32_t color_space;
    int32_t trd_right_fd;
    bool pre_multiply;
    struct tsp_disp_rect64 crop; /* 32.32 fixed point */
    int32_t flags;
    int32_t scan;
    int32_t eotf;
    int32_t depth;
    uint32_t fbd_en;
    int32_t metadata_fd;
    uint32_t metadata_size;
    uint32_t metadata_flag;
};

struct tsp_disp_atw_info {
    bool used;
    int32_t mode;
    uint32_t b_row;
    uint32_t b_col;
    int32_t cof_fd;
};

struct tsp_disp_snr_info {
    uint8_t en;
    uint8_t demo_en;
    struct tsp_disp_rect demo_win;
    uint8_t y_strength;
    uint8_t u_strength;
    uint8_t v_strength;
    uint8_t th_ver_line;
    uint8_t th_hor_line;
};

struct tsp_disp_layer_info2 {
    int32_t mode; /* 0 = buffer, 1 = colour */
    uint8_t zorder;
    uint8_t alpha_mode; /* 0 pixel, 1 global, 2 mixed */
    uint8_t alpha_value;
    struct tsp_disp_rect screen_win;
    bool b_trd_out;
    int32_t out_trd_mode;
    union {
        uint32_t color;
        struct tsp_disp_fb_info2 fb;
    };
    uint32_t id;
    struct tsp_disp_atw_info atw;
    int32_t transform;
    struct tsp_disp_snr_info snr;
};

struct tsp_disp_layer_config2 {
    struct tsp_disp_layer_info2 info;
    bool enable;
    uint32_t channel;
    uint32_t layer_id;
};

/*
 * Version 1 layer configuration: takes device (IOMMU) addresses instead of
 * dmabuf fds. The sunxi IOMMU uses one global domain, so addresses that
 * the video engine reports are valid for the display engine too.
 */
struct tsp_disp_fb_info {
    uint64_t addr[3];
    struct tsp_disp_rectsz size[3];
    uint32_t align[3];
    int32_t format;
    int32_t color_space;
    uint32_t trd_right_addr[3];
    bool pre_multiply;
    struct tsp_disp_rect64 crop; /* 32.32 fixed point */
    int32_t flags;
    int32_t scan;
};

struct tsp_disp_layer_info {
    int32_t mode;
    uint8_t zorder;
    uint8_t alpha_mode;
    uint8_t alpha_value;
    struct tsp_disp_rect screen_win;
    bool b_trd_out;
    int32_t out_trd_mode;
    union {
        uint32_t color;
        struct tsp_disp_fb_info fb;
    };
    uint32_t id;
};

struct tsp_disp_layer_config {
    struct tsp_disp_layer_info info;
    bool enable;
    uint32_t channel;
    uint32_t layer_id;
};

_Static_assert(sizeof(struct tsp_disp_fb_info) == 128, "disp_fb_info");
_Static_assert(offsetof(struct tsp_disp_fb_info, crop) == 88, "fb_info.crop");
_Static_assert(offsetof(struct tsp_disp_layer_info, fb) == 32, "info.fb");
_Static_assert(offsetof(struct tsp_disp_layer_info, id) == 160, "info.id");
_Static_assert(sizeof(struct tsp_disp_layer_info) == 168, "layer_info");
_Static_assert(offsetof(struct tsp_disp_layer_config, enable) == 168,
               "config.enable");
_Static_assert(sizeof(struct tsp_disp_layer_config) == 184, "layer_config");

_Static_assert(sizeof(struct tsp_disp_fb_info2) == 120, "disp_fb_info2");
_Static_assert(offsetof(struct tsp_disp_layer_info2, fb) == 32, "info2.fb");
_Static_assert(offsetof(struct tsp_disp_layer_info2, id) == 152, "info2.id");
_Static_assert(offsetof(struct tsp_disp_layer_info2, transform) == 176,
               "info2.transform");
_Static_assert(sizeof(struct tsp_disp_layer_info2) == 208, "layer_info2");
_Static_assert(offsetof(struct tsp_disp_layer_config2, enable) == 208,
               "config2.enable");
_Static_assert(offsetof(struct tsp_disp_layer_config2, channel) == 212,
               "config2.channel");
_Static_assert(sizeof(struct tsp_disp_layer_config2) == 224, "layer_config2");

#endif
