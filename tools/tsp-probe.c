/*
 * tsp-probe: read-only check of the disp2 layer ABI on the running kernel.
 *
 * Reads every layer of screen 0 with DISP_LAYER_GET_CONFIG2 into a
 * 0xA5-filled buffer and prints what the kernel filled in. The existing
 * framebuffer layer must decode to sane values at the offsets declared in
 * sunxi_disp_abi.h. Nothing is written to the display engine.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "sunxi_disp_abi.h"

#define PROBE_BYTES 4096

static int get_layer(int fd, unsigned ch, unsigned lyr, unsigned char *buf)
{
    struct tsp_disp_layer_config2 *cfg = (void *)buf;
    unsigned long args[4];

    memset(buf, 0xa5, PROBE_BYTES);
    cfg->channel = ch;
    cfg->layer_id = lyr;
    args[0] = 0;
    args[1] = (unsigned long)buf;
    args[2] = 1;
    args[3] = 0;
    return ioctl(fd, TSP_DISP_LAYER_GET_CONFIG2, args);
}

int main(void)
{
    unsigned char *buf;
    unsigned long args[4] = { 0, 0, 0, 0 };
    int fd, w, h, ok = 0;
    unsigned ch, lyr;

    printf("abi: fb_info2=%zu layer_info2=%zu layer_config2=%zu\n",
           sizeof(struct tsp_disp_fb_info2),
           sizeof(struct tsp_disp_layer_info2),
           sizeof(struct tsp_disp_layer_config2));

    fd = open("/dev/disp", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/disp");
        return 1;
    }
    w = ioctl(fd, TSP_DISP_GET_SCN_WIDTH, args);
    h = ioctl(fd, TSP_DISP_GET_SCN_HEIGHT, args);
    printf("screen0: %dx%d\n", w, h);

    buf = aligned_alloc(64, PROBE_BYTES);
    if (buf == NULL)
        return 1;

    for (ch = 0; ch < 4; ch++) {
        for (lyr = 0; lyr < 4; lyr++) {
            struct tsp_disp_layer_config2 *c = (void *)buf;
            const struct tsp_disp_fb_info2 *fb = &c->info.fb;
            unsigned tail = 0;
            int ret = get_layer(fd, ch, lyr, buf);
            size_t i;

            for (i = sizeof(*c); i < sizeof(*c) + 64; i++)
                if (buf[i] != 0xa5)
                    tail++;
            printf("ch%u lyr%u ret=%d enable=0x%02x chan=%u layer=%u "
                   "mode=%d z=%u alpha=%u/%u win=%d,%d %ux%u "
                   "fb.fd=%d size0=%ux%u size1=%ux%u align=%u,%u "
                   "fmt=0x%x cs=0x%x crop=%lld,%lld %lldx%lld "
                   "eotf=0x%x depth=%d fbd=%u meta_fd=%d tail_changed=%u\n",
                   ch, lyr, ret, *(unsigned char *)&c->enable, c->channel,
                   c->layer_id, c->info.mode, c->info.zorder,
                   c->info.alpha_mode, c->info.alpha_value,
                   c->info.screen_win.x, c->info.screen_win.y,
                   c->info.screen_win.width, c->info.screen_win.height,
                   fb->fd, fb->size[0].width, fb->size[0].height,
                   fb->size[1].width, fb->size[1].height, fb->align[0],
                   fb->align[1], fb->format, fb->color_space,
                   (long long)(fb->crop.x >> 32), (long long)(fb->crop.y >> 32),
                   (long long)(fb->crop.width >> 32),
                   (long long)(fb->crop.height >> 32), fb->eotf, fb->depth,
                   fb->fbd_en, fb->metadata_fd, tail);
            if (ret == 0 && *(unsigned char *)&c->enable == 1 &&
                c->channel == ch &&
                c->layer_id == lyr && fb->size[0].width == (unsigned)w &&
                fb->size[0].height == (unsigned)h)
                ok = 1;
        }
    }
    /* Version 1 view of the framebuffer layer: must report its address. */
    {
        struct tsp_disp_layer_config *c = (void *)buf;
        const struct tsp_disp_fb_info *fb = &c->info.fb;
        unsigned long a[4] = { 0, (unsigned long)buf, 1, 0 };
        int ret;

        memset(buf, 0xa5, PROBE_BYTES);
        c->channel = 1;
        c->layer_id = 0;
        ret = ioctl(fd, TSP_DISP_LAYER_GET_CONFIG, a);
        printf("v1 ch1 lyr0 ret=%d enable=0x%02x chan=%u layer=%u z=%u "
               "win=%ux%u addr=0x%llx,0x%llx size0=%ux%u fmt=0x%x "
               "cs=0x%x crop=%lldx%lld\n",
               ret, *(unsigned char *)&c->enable, c->channel, c->layer_id,
               c->info.zorder, c->info.screen_win.width,
               c->info.screen_win.height, (unsigned long long)fb->addr[0],
               (unsigned long long)fb->addr[1], fb->size[0].width,
               fb->size[0].height, fb->format, fb->color_space,
               (long long)(fb->crop.width >> 32),
               (long long)(fb->crop.height >> 32));
        if (!(ret == 0 && *(unsigned char *)&c->enable == 1 &&
              c->channel == 1 && fb->size[0].width == (unsigned)w &&
              fb->size[0].height == (unsigned)h && fb->addr[0] != 0 &&
              (fb->crop.width >> 32) == w))
            ok = 0;
    }
    free(buf);
    close(fd);
    printf("layout: %s\n", ok ? "verified (fb layer decoded, v1 and v2)" :
                                "NOT verified");
    return ok ? 0 : 2;
}
