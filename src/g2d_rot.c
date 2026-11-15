/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "g2d_rot.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sunxi_g2d_abi.h"

void g2drot_unmap_iova(struct g2drot *r)
{
    unsigned i;

    for (i = 0; i < G2DROT_MAX_BUFS; i++) {
        struct g2drot_buf *b = &r->buf[i];

        if (b->iova != 0 && r->cedar_fd >= 0) {
            struct tsp_cedar_iommu_param p = { .fd = b->ion.fd };

            /* Fails harmlessly if the decoder's release already did it. */
            ioctl(r->cedar_fd, TSP_CEDAR_IOCTL_FREE_IOMMU_ADDR, &p);
        }
        b->iova = 0;
    }
    r->cedar_fd = -1;
}

int g2drot_map_iova(struct g2drot *r, int cedar_fd)
{
    unsigned i;

    r->cedar_fd = cedar_fd;
    for (i = 0; i < r->nbufs; i++) {
        struct tsp_cedar_iommu_param p = { .fd = r->buf[i].ion.fd };

        if (ioctl(cedar_fd, TSP_CEDAR_IOCTL_GET_IOMMU_ADDR, &p) != 0 ||
            p.iommu_addr == 0) {
            int saved = errno;

            g2drot_unmap_iova(r);
            errno = saved ? saved : EIO;
            return -1;
        }
        r->buf[i].iova = p.iommu_addr;
    }
    return 0;
}

int g2drot_open(struct g2drot *r, unsigned nbufs, unsigned dst_w,
                unsigned dst_h, unsigned heap_id, int map_for_cpu)
{
    unsigned i;

    memset(r, 0, sizeof(*r));
    r->g2d_fd = r->ion_fd = r->cedar_fd = -1;
    for (i = 0; i < G2DROT_MAX_BUFS; i++)
        r->buf[i].ion.fd = -1;
    if (nbufs < 2 || nbufs > G2DROT_MAX_BUFS || dst_w == 0 || dst_h == 0 ||
        (dst_w & 1) || (dst_h & 1)) {
        errno = EINVAL;
        return -1;
    }
    r->nbufs = nbufs;
    r->dst_w = dst_w;
    r->dst_h = dst_h;

    r->g2d_fd = open("/dev/g2d", O_RDWR | O_CLOEXEC);
    r->ion_fd = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (r->g2d_fd < 0 || r->ion_fd < 0)
        goto fail;

    /* Uncached: only the hardware touches them. */
    for (i = 0; i < nbufs; i++)
        if (ionbuf_alloc(r->ion_fd, &r->buf[i].ion,
                         (size_t)dst_w * dst_h * 3 / 2, heap_id,
                         map_for_cpu ? PROT_READ : 0) != 0)
            goto fail;
    return 0;

fail:
    {
        int saved = errno;

        g2drot_close(r);
        errno = saved;
    }
    return -1;
}

void g2drot_close(struct g2drot *r)
{
    unsigned i;

    g2drot_unmap_iova(r);
    for (i = 0; i < G2DROT_MAX_BUFS; i++)
        ionbuf_free(r->ion_fd, &r->buf[i].ion);
    if (r->g2d_fd >= 0)
        close(r->g2d_fd);
    if (r->ion_fd >= 0)
        close(r->ion_fd);
    r->g2d_fd = r->ion_fd = -1;
}

int g2drot_rotate(struct g2drot *r, const struct g2drot_src *src,
                  int rot_flag)
{
    struct tsp_g2d_blt_h blt;
    int fmt = src->nv21 ? TSP_G2D_FORMAT_YUV420UVC_U1V1U0V0
                        : TSP_G2D_FORMAT_YUV420UVC_V1U1V0U0;
    unsigned idx = r->next;

    if ((rot_flag == TSP_G2D_ROT_90 || rot_flag == TSP_G2D_ROT_270)
            ? (src->crop_w != r->dst_h || src->crop_h != r->dst_w)
            : (src->crop_w != r->dst_w || src->crop_h != r->dst_h)) {
        errno = EINVAL;
        return -1;
    }
    if (src->addr_y > 0xffffffffffull || src->addr_c > 0xffffffffffull) {
        errno = EINVAL;
        return -1;
    }

    memset(&blt, 0, sizeof(blt));
    blt.flag_h = rot_flag;

    blt.src_image_h.format = fmt;
    blt.src_image_h.laddr[0] = (uint32_t)src->addr_y;
    blt.src_image_h.haddr[0] = (uint32_t)(src->addr_y >> 32);
    blt.src_image_h.laddr[1] = (uint32_t)src->addr_c;
    blt.src_image_h.haddr[1] = (uint32_t)(src->addr_c >> 32);
    blt.src_image_h.width = src->stride;
    blt.src_image_h.height = src->alloc_h;
    blt.src_image_h.clip_rect.x = (int32_t)src->crop_x;
    blt.src_image_h.clip_rect.y = (int32_t)src->crop_y;
    blt.src_image_h.clip_rect.w = src->crop_w;
    blt.src_image_h.clip_rect.h = src->crop_h;
    blt.src_image_h.alpha = 0xff;
    blt.src_image_h.fd = -1;
    blt.src_image_h.use_phy_addr = 1;

    blt.dst_image_h.format = fmt;
    blt.dst_image_h.width = r->dst_w;
    blt.dst_image_h.height = r->dst_h;
    blt.dst_image_h.clip_rect.w = r->dst_w;
    blt.dst_image_h.clip_rect.h = r->dst_h;
    blt.dst_image_h.alpha = 0xff;
    if (r->buf[idx].iova != 0) {
        uint32_t c = r->buf[idx].iova + r->dst_w * r->dst_h;

        blt.dst_image_h.laddr[0] = r->buf[idx].iova;
        blt.dst_image_h.laddr[1] = c;
        blt.dst_image_h.fd = -1;
        blt.dst_image_h.use_phy_addr = 1;
    } else {
        /* The driver maps the dmabuf and derives the chroma address. */
        blt.dst_image_h.fd = r->buf[idx].ion.fd;
        blt.dst_image_h.use_phy_addr = 0;
    }

    if (ioctl(r->g2d_fd, TSP_G2D_CMD_BITBLT_H, &blt) != 0)
        return -1;
    r->next = (idx + 1) % r->nbufs;
    return (int)idx;
}
