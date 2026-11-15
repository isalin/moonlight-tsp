/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "disp.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sunxi_g2d_abi.h"

static int disp_ioctl(int fd, unsigned long cmd, unsigned long a0,
                      unsigned long a1, unsigned long a2)
{
    unsigned long args[4] = { a0, a1, a2, 0 };

    return ioctl(fd, cmd, args);
}

int dispv_get_config(int fd, unsigned screen, unsigned channel,
                     unsigned layer, struct tsp_disp_layer_config2 *out)
{
    memset(out, 0, sizeof(*out));
    out->channel = channel;
    out->layer_id = layer;
    return disp_ioctl(fd, TSP_DISP_LAYER_GET_CONFIG2, screen,
                      (unsigned long)out, 1);
}

int dispv_disable_layer(int fd, unsigned screen, unsigned channel,
                        unsigned layer)
{
    struct tsp_disp_layer_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.enable = false;
    cfg.channel = channel;
    cfg.layer_id = layer;
    return disp_ioctl(fd, TSP_DISP_LAYER_SET_CONFIG, screen,
                      (unsigned long)&cfg, 1);
}

static int dispv_show_addr(struct dispv *d, const struct dispv_frame *f)
{
    struct tsp_disp_layer_config cfg;
    struct tsp_disp_fb_info *fb = &cfg.info.fb;

    memset(&cfg, 0, sizeof(cfg));
    cfg.enable = true;
    cfg.channel = d->channel;
    cfg.layer_id = d->layer;
    cfg.info.mode = 0;
    cfg.info.zorder = (uint8_t)d->zorder;
    cfg.info.alpha_mode = 1;
    cfg.info.alpha_value = 0xff;
    cfg.info.screen_win = f->win;

    fb->addr[0] = f->addr_y;
    fb->addr[1] = f->addr_c;
    fb->format = f->format;
    fb->color_space = f->color_space;
    fb->size[0].width = f->stride;
    fb->size[0].height = f->alloc_h;
    fb->size[1].width = f->stride / 2;
    fb->size[1].height = f->alloc_h / 2;
    fb->size[2] = fb->size[1];
    fb->crop.x = (int64_t)f->crop_x << 32;
    fb->crop.y = (int64_t)f->crop_y << 32;
    fb->crop.width = (int64_t)f->crop_w << 32;
    fb->crop.height = (int64_t)f->crop_h << 32;

    if (disp_ioctl(d->fd, TSP_DISP_LAYER_SET_CONFIG, d->screen,
                   (unsigned long)&cfg, 1) != 0)
        return -1;
    d->enabled = 1;
    return 0;
}

int dispv_open(struct dispv *d, unsigned channel, unsigned layer,
               unsigned zorder)
{
    int w, h;

    memset(d, 0, sizeof(*d));
    d->fd = open("/dev/disp", O_RDWR | O_CLOEXEC);
    if (d->fd < 0)
        return -1;
    d->screen = 0;
    d->channel = channel;
    d->layer = layer;
    d->zorder = zorder;

    w = disp_ioctl(d->fd, TSP_DISP_GET_SCN_WIDTH, 0, 0, 0);
    h = disp_ioctl(d->fd, TSP_DISP_GET_SCN_HEIGHT, 0, 0, 0);
    if (w <= 0 || h <= 0) {
        close(d->fd);
        d->fd = -1;
        errno = ENODEV;
        return -1;
    }
    d->screen_w = (unsigned)w;
    d->screen_h = (unsigned)h;
    return 0;
}

int dispv_show(struct dispv *d, const struct dispv_frame *f)
{
    struct tsp_disp_layer_config2 cfg;
    struct tsp_disp_fb_info2 *fb = &cfg.info.fb;

    if (f->dmabuf_fd < 0)
        return dispv_show_addr(d, f);

    memset(&cfg, 0, sizeof(cfg));
    cfg.enable = true;
    cfg.channel = d->channel;
    cfg.layer_id = d->layer;
    cfg.info.mode = 0;
    cfg.info.zorder = (uint8_t)d->zorder;
    cfg.info.alpha_mode = 1;
    cfg.info.alpha_value = 0xff;
    cfg.info.screen_win = f->win;

    fb->fd = f->dmabuf_fd;
    fb->format = f->format;
    fb->color_space = f->color_space;
    fb->trd_right_fd = -1;
    fb->metadata_fd = -1;
    fb->eotf = TSP_DISP_EOTF_GAMMA22;
    /*
     * disp_set_fb_info(): luma pitch = size[0].width bytes (align 0),
     * chroma starts size[0].width * size[0].height bytes in.
     */
    fb->size[0].width = f->stride;
    fb->size[0].height = f->alloc_h;
    fb->size[1].width = f->stride / 2;
    fb->size[1].height = f->alloc_h / 2;
    fb->size[2] = fb->size[1];
    fb->crop.x = (int64_t)f->crop_x << 32;
    fb->crop.y = (int64_t)f->crop_y << 32;
    fb->crop.width = (int64_t)f->crop_w << 32;
    fb->crop.height = (int64_t)f->crop_h << 32;

    if (disp_ioctl(d->fd, TSP_DISP_LAYER_SET_CONFIG2, d->screen,
                   (unsigned long)&cfg, 1) != 0)
        return -1;
    d->enabled = 1;
    return 0;
}

int dispv_hide(struct dispv *d)
{
    if (d->fd < 0)
        return 0;
    d->enabled = 0;
    return dispv_disable_layer(d->fd, d->screen, d->channel, d->layer);
}

int dispv_flush_imports(struct dispv *d, int dummy_fd)
{
    struct tsp_disp_layer_config2 cfg;
    struct tsp_disp_fb_info2 *fb = &cfg.info.fb;
    int i, rc = 0;

    if (d->fd < 0)
        return 0;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enable = true;
    cfg.channel = d->channel;
    cfg.layer_id = d->layer;
    cfg.info.zorder = (uint8_t)d->zorder;
    cfg.info.alpha_mode = 1;
    cfg.info.alpha_value = 0;
    cfg.info.screen_win.width = DISPV_DUMMY_SIZE;
    cfg.info.screen_win.height = DISPV_DUMMY_SIZE;
    fb->fd = dummy_fd;
    fb->format = TSP_DISP_FORMAT_YUV420_SP_UVUV;
    fb->color_space = TSP_DISP_BT601;
    fb->trd_right_fd = -1;
    fb->metadata_fd = -1;
    fb->eotf = TSP_DISP_EOTF_GAMMA22;
    fb->size[0].width = fb->size[0].height = DISPV_DUMMY_SIZE;
    fb->size[1].width = fb->size[1].height = DISPV_DUMMY_SIZE / 2;
    fb->size[2] = fb->size[1];
    fb->crop.width = (int64_t)DISPV_DUMMY_SIZE << 32;
    fb->crop.height = (int64_t)DISPV_DUMMY_SIZE << 32;

    /* One commit per vsync: the driver tags imports with the vsync count. */
    for (i = 0; i < DISPV_IMPORT_KEEP + 2; i++) {
        if (disp_ioctl(d->fd, TSP_DISP_LAYER_SET_CONFIG2, d->screen,
                       (unsigned long)&cfg, 1) != 0) {
            rc = -1;
            break;
        }
        d->enabled = 1;
        usleep(20000);
    }
    if (dispv_hide(d) != 0)
        rc = -1;
    return rc;
}

int dispv_alloc_dummy(int ion_fd, struct ionbuf *b)
{
    const size_t luma = DISPV_DUMMY_SIZE * DISPV_DUMMY_SIZE;

    if (ionbuf_alloc(ion_fd, b, luma * 3 / 2, TSP_ION_HEAP_SYS_USER,
                     PROT_READ | PROT_WRITE) != 0)
        return -1;
    memset(b->map, 16, luma);                       /* black */
    memset((char *)b->map + luma, 128, luma / 2);
    return 0;
}

int dispv_reset(unsigned channel, unsigned layer, unsigned zorder, int flush)
{
    struct dispv d;
    struct ionbuf dummy = IONBUF_INIT;
    int ion_fd = -1, rc;

    if (dispv_open(&d, channel, layer, zorder) != 0)
        return -1;
    if (flush) {
        ion_fd = open("/dev/ion", O_RDWR | O_CLOEXEC);
        if (ion_fd < 0 || dispv_alloc_dummy(ion_fd, &dummy) != 0)
            flush = 0; /* still disable the layer */
    }
    rc = flush ? dispv_flush_imports(&d, dummy.fd) : dispv_hide(&d);
    ionbuf_free(ion_fd, &dummy);
    if (ion_fd >= 0)
        close(ion_fd);
    close(d.fd);
    return rc;
}

void dispv_close(struct dispv *d)
{
    if (d->fd < 0)
        return;
    if (d->enabled)
        dispv_hide(d);
    close(d->fd);
    d->fd = -1;
}

struct tsp_disp_rect dispv_fit(unsigned screen_w, unsigned screen_h,
                               unsigned w, unsigned h)
{
    struct tsp_disp_rect r;

    if (w == 0 || h == 0) {
        r.x = r.y = 0;
        r.width = screen_w;
        r.height = screen_h;
        return r;
    }
    if ((uint64_t)screen_w * h <= (uint64_t)screen_h * w) {
        r.width = screen_w;
        r.height = (unsigned)((uint64_t)screen_w * h / w) & ~1u;
    } else {
        r.height = screen_h;
        r.width = (unsigned)((uint64_t)screen_h * w / h) & ~1u;
    }
    r.x = (int)(screen_w - r.width) / 2;
    r.y = (int)(screen_h - r.height) / 2;
    return r;
}
