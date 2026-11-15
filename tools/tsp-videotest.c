/*
 * tsp-videotest: Cedar hardware decode + display-engine layer spike.
 *
 * Decodes an Annex-B H.264/H.265 elementary stream (one access unit per
 * AUD) with the vendor CedarC decoder and optionally shows every picture
 * on a disp2 video layer using the picture's own dmabuf. Reports
 * per-frame submit-to-picture latency, display ioctl time and CPU use.
 *
 * Safety: the layer is disabled on normal exit, SIGINT, SIGTERM and from
 * a SIGALRM watchdog that also fires if decoding wedges.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cedar.h"
#include "cedar_sync.h"
#include "disp.h"
#include "g2d_rot.h"
#include "ion.h"
#include "sunxi_g2d_abi.h"

#define MAX_HELD 8

struct au {
    size_t off;
    size_t len;
};

struct held_pic {
    CedarVideoPicture *pic;
    uint64_t retired_ns; /* 0 while still on screen */
};

static volatile sig_atomic_t g_stop;
static int g_disp_fd = -1;
static unsigned long g_disable_args[4];
static struct tsp_disp_layer_config g_disable_cfg;

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void emergency_disable(void)
{
    if (g_disp_fd >= 0)
        ioctl(g_disp_fd, TSP_DISP_LAYER_SET_CONFIG, g_disable_args);
}

static void on_signal(int sig)
{
    if (sig == SIGALRM || g_stop) {
        static const char msg[] = "tsp-videotest: watchdog/second signal, "
                                  "disabling layer and exiting\n";
        emergency_disable();
        (void)!write(2, msg, sizeof(msg) - 1);
        _exit(3);
    }
    g_stop = 1;
    alarm(3);
}

static int is_aud(const unsigned char *nal, int hevc)
{
    if (hevc)
        return ((nal[0] >> 1) & 0x3f) == 35;
    return (nal[0] & 0x1f) == 9;
}

static size_t split_access_units(const unsigned char *data, size_t len,
                                 int hevc, struct au **out)
{
    size_t cap = 1024, n = 0, i;
    struct au *aus = malloc(cap * sizeof(*aus));

    if (aus == NULL)
        return 0;
    for (i = 0; i + 3 < len; i++) {
        size_t start;

        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
            continue;
        if (!is_aud(data + i + 3, hevc)) {
            i += 2;
            continue;
        }
        start = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
        if (n == cap) {
            struct au *tmp;

            cap *= 2;
            tmp = realloc(aus, cap * sizeof(*aus));
            if (tmp == NULL) {
                free(aus);
                return 0;
            }
            aus = tmp;
        }
        if (n > 0)
            aus[n - 1].len = start - aus[n - 1].off;
        aus[n].off = start;
        aus[n].len = 0;
        n++;
        i += 2;
    }
    if (n > 0)
        aus[n - 1].len = len - aus[n - 1].off;
    *out = aus;
    return n;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    struct stat st;
    unsigned char *buf;
    size_t got = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    buf = malloc((size_t)st.st_size);
    while (buf != NULL && got < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);

        if (r <= 0) {
            free(buf);
            buf = NULL;
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    *len = got;
    return buf;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

static void report_series(const char *name, float *v, size_t n)
{
    double sum = 0;
    size_t i;

    if (n == 0) {
        printf("%s: no samples\n", name);
        return;
    }
    for (i = 0; i < n; i++)
        sum += v[i];
    qsort(v, n, sizeof(*v), cmp_float);
    printf("%s: n=%zu mean=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f ms\n",
           name, n, sum / (double)n, v[n / 2], v[(n * 95) / 100],
           v[(n * 99) / 100], v[n - 1]);
}

static double cpu_seconds(void)
{
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
           (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

static long rss_kib(void)
{
    char line[128];
    long kib = -1;
    FILE *f = fopen("/proc/self/status", "re");

    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL)
        if (sscanf(line, "VmRSS: %ld", &kib) == 1)
            break;
    fclose(f);
    return kib;
}

static void dump_picture(const CedarVideoPicture *p)
{
    printf("picture: id=%d fmt=%d %dx%d stride=%d offs l=%d t=%d r=%d b=%d "
           "progressive=%d phyY=0x%llx phyC=0x%llx (C-Y=%lld) bufFd=%d "
           "bufId=%d bufSize=%d full_range=%d matrix=%d primaries=%u "
           "afbc=%d lbc=%d 10bit=%d pData0=%p pData1=%p\n",
           p->nID, p->ePixelFormat, p->nWidth, p->nHeight, p->nLineStride,
           p->nLeftOffset, p->nTopOffset, p->nRightOffset, p->nBottomOffset,
           p->bIsProgressive, (unsigned long long)p->phyYBufAddr,
           (unsigned long long)p->phyCBufAddr,
           (long long)(p->phyCBufAddr - p->phyYBufAddr), p->nBufFd,
           p->nBufId, p->nBufSize, p->video_full_range_flag,
           p->matrix_coeffs, p->colour_primaries, p->bEnableAfbcFlag,
           p->nLbcSize, p->b10BitPicFlag, (void *)p->pData0,
           (void *)p->pData1);
}

/* Writes a w x h window of an 8-bit plane as a binary PGM. */
static int dump_plane(const char *path, const unsigned char *plane,
                      unsigned stride, unsigned x0, unsigned y0, unsigned w,
                      unsigned h)
{
    FILE *f;
    unsigned row;

    if (plane == NULL || stride == 0 || x0 + w > stride)
        return -1;
    f = fopen(path, "we");
    if (f == NULL)
        return -1;
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    for (row = 0; row < h; row++)
        fwrite(plane + (size_t)(y0 + row) * stride + x0, 1, w, f);
    return fclose(f) == 0 ? 0 : -1;
}

/* Writes the visible luma plane of a decoded picture as a binary PGM. */
static int dump_luma(const CedarVideoPicture *p, const char *path)
{
    unsigned x0 = 0, y0 = 0, w = (unsigned)p->nWidth, h = (unsigned)p->nHeight;

    if (p->nLineStride <= 0)
        return -1;
    if (p->nRightOffset > p->nLeftOffset && p->nBottomOffset > p->nTopOffset) {
        x0 = (unsigned)p->nLeftOffset;
        y0 = (unsigned)p->nTopOffset;
        w = (unsigned)(p->nRightOffset - p->nLeftOffset);
        h = (unsigned)(p->nBottomOffset - p->nTopOffset);
    }
    return dump_plane(path, (const unsigned char *)p->pData0,
                      (unsigned)p->nLineStride, x0, y0, w, h);
}

/*
 * Frames behind the newest submission, from the marker block luma
 * 16 + 8 * (frame % 27). au_index is the in-stream index just submitted.
 */
static int marker_lag(const unsigned char *plane, unsigned stride,
                      unsigned x, unsigned y, size_t au_index, unsigned *luma)
{
    unsigned v = *(const volatile unsigned char *)(plane +
                                                   (size_t)y * stride + x);
    int k = ((int)v - 16 + 4) / 8;
    int lag = (int)(au_index % 27) - k;

    if (lag < 0)
        lag += 27;
    *luma = v;
    return lag;
}

static int picture_to_frame(const CedarVideoPicture *p, int color_space,
                            const struct dispv *d, struct dispv_frame *f)
{
    unsigned stride = (unsigned)p->nLineStride;
    unsigned w, h;

    memset(f, 0, sizeof(*f));
    if (p->ePixelFormat == CEDAR_PIXEL_NV12)
        f->format = TSP_DISP_FORMAT_YUV420_SP_UVUV;
    else if (p->ePixelFormat == CEDAR_PIXEL_NV21)
        f->format = TSP_DISP_FORMAT_YUV420_SP_VUVU;
    else
        return -1;
    if (p->phyYBufAddr == 0 || p->phyCBufAddr == 0 || stride == 0)
        return -1;

    /*
     * CedarC's own picture buffers carry no dmabuf fd (nBufFd is 0), but
     * their phy addresses are IOVAs in the single sunxi IOMMU domain that
     * the display engine shares, so the v1 address interface works.
     */
    f->dmabuf_fd = -1;
    f->addr_y = p->phyYBufAddr;
    f->addr_c = p->phyCBufAddr;
    f->color_space = color_space;
    f->stride = stride;
    if (p->phyCBufAddr > p->phyYBufAddr)
        f->alloc_h = (unsigned)((p->phyCBufAddr - p->phyYBufAddr) / stride);
    else
        f->alloc_h = ((unsigned)p->nHeight + 15u) & ~15u;

    if (p->nRightOffset > p->nLeftOffset &&
        p->nBottomOffset > p->nTopOffset) {
        f->crop_x = (unsigned)p->nLeftOffset;
        f->crop_y = (unsigned)p->nTopOffset;
        w = (unsigned)(p->nRightOffset - p->nLeftOffset);
        h = (unsigned)(p->nBottomOffset - p->nTopOffset);
    } else {
        w = (unsigned)p->nWidth;
        h = (unsigned)p->nHeight;
    }
    f->crop_w = w;
    f->crop_h = h;
    f->win = dispv_fit(d->screen_w, d->screen_h, w, h);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options] stream.264|stream.265\n"
            "  -c h264|h265      codec (default from file extension)\n"
            "  -s WxH            coded size hint (default 1280x720)\n"
            "  -r FPS            submit pacing, 0 = unpaced (default 60)\n"
            "  -l LOOPS          repeat count (default 1)\n"
            "  -t SECONDS        stop after this long (default 30)\n"
            "  -d                show pictures on a display layer\n"
            "  -R N              Cedar nRotateDegree index (default 0)\n"
            "  -f nv12|nv21      decoder output (default nv21)\n"
            "  -C CH -L LYR -z Z layer position (default 0 0 32)\n"
            "  -k MS             keep a replaced picture this long (34)\n"
            "  -H N              display hold pictures (default 3)\n"
            "  -S N              decode smooth pictures (default 1)\n"
            "  -o CS             disp color space (default 0x104 BT601)\n"
            "  -A                append an access unit delimiter to each\n"
            "                    frame so the parser closes it at once\n"
            "  -P HEX            bytes to append instead of the AUD (-A)\n"
            "  -W MS             retry no-bitstream decodes up to MS ms\n"
            "  -K                with -W: sleep until the parser thread\n"
            "                    blocks instead of polling every 200 us\n"
            "  -M                read the frame-number marker block\n"
            "                    (make-test-streams.sh) to measure lag\n"
            "  -m X,Y            marker sample position (default 32,32)\n"
            "  -D FILE           write picture -N's luma as PGM\n"
            "  -N N              picture to dump (default 30)\n"
            "  -G 90|270         rotate clockwise with G2D into ION buffers\n"
            "  -I sys|cma        ION heap for -G buffers (default sys)\n"
            "  -B N              -G ring buffers (default 3)\n"
            "  -V                -G: map buffers into the IOMMU once\n"
            "  -1                -G -V: display by address (v1 layer)\n"
            "  -F                -d, -X: flush the driver's dmabuf imports\n"
            "  -v                print every picture\n"
            "  -X                only disable layer -C/-L and exit\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct cedar_config cfg = {
        .codec = 0,
        .width = 1280,
        .height = 720,
        .fps = 60,
        .pixel_format = CEDAR_PIXEL_NV21,
        .rotate = 0,
        .display_hold = 3,
        .smooth = 1,
        .vbv_bytes = 4 << 20,
    };
    unsigned channel = 0, layer = 0, zorder = 32;
    int display = 0, verbose = 0, loops = 1, seconds = 30, reset_only = 0;
    int color_space = TSP_DISP_BT601;
    double pace_fps = 60, keep_ms = 34, wait_ms = 0;
    int append_aud = 0, marker = 0, kick = 0;
    unsigned mark_x = 32, mark_y = 32;
    const char *dump_path = NULL;
    unsigned long dump_at = 30;
    int g2d_deg = 0, g2d_flag = 0;
    unsigned g2d_heap = TSP_ION_HEAP_SYS_USER, g2d_nbufs = 3;
    struct g2drot rot = { .g2d_fd = -1, .ion_fd = -1 };
    int rot_open = 0, rot_iova = 0, rot_v1 = 0, own_cedar_fd = -1;
    int flush = 0, dummy_ion_fd = -1;
    struct ionbuf dummy = IONBUF_INIT;
    float *rot_ms = NULL;
    size_t nrot = 0;
    unsigned long mark_lag[10] = { 0 };
    unsigned char suffix[64];
    size_t suffix_len = 0;
    static const unsigned char aud_h264[] = { 0, 0, 0, 1, 0x09, 0xf0 };
    static const unsigned char aud_h265[] = { 0, 0, 0, 1, 0x46, 0x01, 0x50 };
    unsigned long waits = 0, decoded_total = 0;
    struct held_pic held[MAX_HELD];
    int nheld = 0;
    char err[256];
    struct cedar_decoder *dec;
    struct dispv disp = { .fd = -1 };
    unsigned char *data;
    size_t len, nau, total_frames, i;
    struct au *aus = NULL;
    float *lat_ms = NULL, *show_ms = NULL, *true_ms = NULL;
    float *hw_ms = NULL, *pre_ms = NULL;
    size_t nhw = 0;
    uint64_t *submit_ns = NULL;
    size_t nlat = 0, nshow = 0, ntrue = 0;
    unsigned long results[8] = { 0 }, lag_max = 0, lag_last = 0,
                  sbm_max = 0;
    uint64_t t_start, next_due, t_last_report;
    double cpu_start;
    long rss_start;
    unsigned long submitted = 0, pictures = 0, shown = 0, errors = 0,
                  nobuf = 0, nopic = 0, frame_err = 0, first_pic_after = 0;
    int opt, loop, rc = 0;
    const char *path;

    while ((opt = getopt(argc, argv, "c:s:r:l:t:dR:f:C:L:z:k:H:S:o:vXAW:MP:Km:D:N:G:I:B:V1F")) != -1) {
        switch (opt) {
        case 'c':
            cfg.codec = strcmp(optarg, "h265") == 0 ? CEDAR_CODEC_H265
                                                    : CEDAR_CODEC_H264;
            break;
        case 's':
            if (sscanf(optarg, "%dx%d", &cfg.width, &cfg.height) != 2) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'r': pace_fps = atof(optarg); break;
        case 'l': loops = atoi(optarg); break;
        case 't': seconds = atoi(optarg); break;
        case 'd': display = 1; break;
        case 'R': cfg.rotate = atoi(optarg); break;
        case 'f':
            cfg.pixel_format = strcmp(optarg, "nv12") == 0 ? CEDAR_PIXEL_NV12
                                                           : CEDAR_PIXEL_NV21;
            break;
        case 'C': channel = (unsigned)atoi(optarg); break;
        case 'L': layer = (unsigned)atoi(optarg); break;
        case 'z': zorder = (unsigned)atoi(optarg); break;
        case 'k': keep_ms = atof(optarg); break;
        case 'H': cfg.display_hold = atoi(optarg); break;
        case 'S': cfg.smooth = atoi(optarg); break;
        case 'o': color_space = (int)strtol(optarg, NULL, 0); break;
        case 'v': verbose = 1; break;
        case 'X': reset_only = 1; break;
        case 'A': append_aud = 1; break;
        case 'M': marker = 1; break;
        case 'P': {
            const char *h = optarg;

            append_aud = 1;
            while (h[0] != '\0' && h[1] != '\0' &&
                   suffix_len < sizeof(suffix)) {
                unsigned v;

                if (sscanf(h, "%2x", &v) != 1) {
                    usage(argv[0]);
                    return 2;
                }
                suffix[suffix_len++] = (unsigned char)v;
                h += 2;
            }
            break;
        }
        case 'W': wait_ms = atof(optarg); break;
        case 'K': kick = 1; break;
        case 'm':
            if (sscanf(optarg, "%u,%u", &mark_x, &mark_y) != 2) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'D': dump_path = optarg; break;
        case 'N': dump_at = strtoul(optarg, NULL, 0); break;
        case 'G':
            g2d_deg = atoi(optarg);
            if (g2d_deg != 90 && g2d_deg != 270) {
                usage(argv[0]);
                return 2;
            }
            g2d_flag = g2d_deg == 90 ? TSP_G2D_ROT_90 : TSP_G2D_ROT_270;
            break;
        case 'I':
            g2d_heap = strcmp(optarg, "cma") == 0 ? TSP_ION_HEAP_CMA
                                                  : TSP_ION_HEAP_SYS_USER;
            break;
        case 'B': g2d_nbufs = (unsigned)atoi(optarg); break;
        case 'V': rot_iova = 1; break;
        case '1': rot_v1 = 1; break;
        case 'F': flush = 1; break;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (reset_only) {
        if (dispv_reset(channel, layer, zorder, flush) != 0) {
            perror("disable layer");
            return 1;
        }
        printf("layer ch%u/%u disabled%s\n", channel, layer,
               flush ? " after flushing imports" : "");
        return 0;
    }
    if (optind != argc - 1) {
        usage(argv[0]);
        return 2;
    }
    path = argv[optind];
    if (cfg.codec == 0) {
        size_t pl = strlen(path);

        cfg.codec = (pl > 4 && (strcmp(path + pl - 4, ".265") == 0 ||
                                strcmp(path + pl - 4, "hevc") == 0))
                        ? CEDAR_CODEC_H265
                        : CEDAR_CODEC_H264;
    }
    cfg.fps = pace_fps > 0 ? (int)(pace_fps + 0.5) : 60;
    if (loops < 1)
        loops = 1;

    data = read_file(path, &len);
    if (data == NULL) {
        perror(path);
        return 1;
    }
    nau = split_access_units(data, len, cfg.codec == CEDAR_CODEC_H265, &aus);
    if (nau == 0) {
        fprintf(stderr, "no access unit delimiters found; encode with "
                        "-bsf:v h264_metadata=aud=insert (or hevc_)\n");
        return 1;
    }
    printf("stream: %s codec=%s bytes=%zu access_units=%zu\n", path,
           cfg.codec == CEDAR_CODEC_H265 ? "h265" : "h264", len, nau);

    total_frames = nau * (size_t)loops;
    lat_ms = calloc(total_frames, sizeof(float));
    show_ms = calloc(total_frames, sizeof(float));
    true_ms = calloc(total_frames, sizeof(float));
    submit_ns = calloc(total_frames, sizeof(uint64_t));
    hw_ms = calloc(total_frames * 2, sizeof(float));
    pre_ms = calloc(total_frames * 2, sizeof(float));
    rot_ms = calloc(total_frames, sizeof(float));
    if (lat_ms == NULL || show_ms == NULL || true_ms == NULL ||
        submit_ns == NULL || hw_ms == NULL || pre_ms == NULL ||
        rot_ms == NULL)
        return 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGALRM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    alarm((unsigned)seconds + 10);

    if (display) {
        if (dispv_open(&disp, channel, layer, zorder) != 0) {
            perror("open /dev/disp");
            return 1;
        }
        g_disable_cfg.enable = false;
        g_disable_cfg.channel = channel;
        g_disable_cfg.layer_id = layer;
        g_disable_args[0] = 0;
        g_disable_args[1] = (unsigned long)&g_disable_cfg;
        g_disable_args[2] = 1;
        g_disp_fd = disp.fd;
        printf("display: screen %ux%u layer ch%u/%u z=%u\n", disp.screen_w,
               disp.screen_h, channel, layer, zorder);
        if (flush) {
            dummy_ion_fd = open("/dev/ion", O_RDWR | O_CLOEXEC);
            if (dummy_ion_fd < 0 ||
                dispv_alloc_dummy(dummy_ion_fd, &dummy) != 0) {
                perror("flush dummy");
                rc = 1;
                goto out;
            }
        }
    }

    if (g2d_flag != 0) {
        /* 1280x720 turned 90 degrees either way is 720x1280. */
        if (g2drot_open(&rot, g2d_nbufs, (unsigned)cfg.height,
                        (unsigned)cfg.width, g2d_heap,
                        dump_path != NULL || marker) != 0) {
            perror("g2d/ion setup");
            rc = 1;
            goto out;
        }
        rot_open = 1;
        printf("g2d: rotate %d cw into %u %ux%u buffers (%s heap)\n",
               g2d_deg, g2d_nbufs, rot.dst_w, rot.dst_h,
               g2d_heap == TSP_ION_HEAP_CMA ? "cma" : "sys_user");
    }

    t_start = now_ns();
    if (cedar_load(err, sizeof(err)) != 0) {
        fprintf(stderr, "cedar_load: %s\n", err);
        rc = 1;
        goto out;
    }
    dec = cedar_open(&cfg, err, sizeof(err));
    if (dec == NULL) {
        fprintf(stderr, "cedar_open: %s\n", err);
        rc = 1;
        goto out;
    }
    printf("decoder: open in %.1f ms (rotate=%d fmt=%d hold=%d smooth=%d)\n",
           (now_ns() - t_start) / 1e6, cfg.rotate, cfg.pixel_format,
           cfg.display_hold, cfg.smooth);
    if (rot_open && rot_iova) {
        /*
         * Mapped after the decoder exists, through our own fd, which stays
         * open until the decoder is gone (closing it would unmap the
         * decoder's buffers too).
         */
        own_cedar_fd = open("/dev/cedar_dev", O_RDWR | O_CLOEXEC);
        if (own_cedar_fd < 0 || g2drot_map_iova(&rot, own_cedar_fd) != 0) {
            perror("cedar IOMMU map");
            rc = 1;
            goto out;
        }
        printf("g2d: buffers mapped once at iova %#x %#x %#x%s\n",
               rot.buf[0].iova, rot.buf[1].iova,
               rot.nbufs > 2 ? rot.buf[2].iova : 0,
               rot_v1 ? ", display by address" : "");
    }

    cpu_start = cpu_seconds();
    rss_start = rss_kib();
    t_start = now_ns();
    next_due = t_start;
    t_last_report = t_start;

    for (loop = 0; loop < loops && !g_stop; loop++) {
        for (i = 0; i < nau && !g_stop; i++) {
            struct cedar_segment seg[2] = {
                { data + aus[i].off, aus[i].len },
                { suffix_len ? suffix
                  : cfg.codec == CEDAR_CODEC_H265 ? aud_h265 : aud_h264,
                  suffix_len ? suffix_len
                  : cfg.codec == CEDAR_CODEC_H265 ? sizeof(aud_h265)
                                                  : sizeof(aud_h264) },
            };
            CedarVideoPicture *pic, *latest = NULL;
            uint64_t t_submit, t_now;
            unsigned seq;
            int ret, tries;

            if ((now_ns() - t_start) / 1000000000ull >= (uint64_t)seconds)
                g_stop = 1;
            if (pace_fps > 0) {
                struct timespec ts;

                ts.tv_sec = (time_t)(next_due / 1000000000ull);
                ts.tv_nsec = (long)(next_due % 1000000000ull);
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
                next_due += (uint64_t)(1e9 / pace_fps);
            }

            seq = cedar_sync_seq();
            t_submit = now_ns();
            submit_ns[submitted] = t_submit;
            ret = cedar_submit(dec, seg, append_aud ? 2 : 1,
                               (int64_t)submitted * 16667);
            if (ret != 0) {
                fprintf(stderr, "submit failed (%d) at au %zu\n", ret, i);
                errors++;
                continue;
            }
            submitted++;

            for (tries = 0; tries < 16; tries++) {
                uint64_t t_call;

                if (kick && wait_ms > 0 && decoded_total < submitted) {
                    /* Sleep until the parser has consumed our data. */
                    cedar_sync_wait(seq, t_submit +
                                             (uint64_t)(wait_ms * 1e6));
                    seq = cedar_sync_seq();
                }
                t_call = now_ns();
                ret = cedar_decode(dec);
                if ((ret == CEDAR_RESULT_FRAME_DECODED ||
                     ret == CEDAR_RESULT_KEYFRAME_DECODED) &&
                    nhw < total_frames * 2) {
                    /* Parser wait vs. hardware decode time. */
                    pre_ms[nhw] = (float)((t_call - t_submit) / 1e6);
                    hw_ms[nhw++] = (float)((now_ns() - t_call) / 1e6);
                }
                if (ret == CEDAR_RESULT_NO_FRAME_BUFFER) {
                    int j, oldest = -1;

                    nobuf++;
                    for (j = 0; j < nheld; j++)
                        if (held[j].retired_ns != 0 &&
                            (oldest < 0 ||
                             held[j].retired_ns < held[oldest].retired_ns))
                            oldest = j;
                    if (oldest < 0) {
                        /* Nothing retired: hold time is too generous. */
                        usleep(2000);
                        continue;
                    }
                    t_now = now_ns();
                    if (t_now < held[oldest].retired_ns +
                                    (uint64_t)(keep_ms * 1e6))
                        usleep((useconds_t)((held[oldest].retired_ns +
                                             (uint64_t)(keep_ms * 1e6) -
                                             t_now) / 1000));
                    cedar_return_picture(dec, held[oldest].pic);
                    held[oldest] = held[--nheld];
                    continue;
                }
                if (ret >= 0 && ret < 8)
                    results[ret]++;
                if (ret == CEDAR_RESULT_FRAME_DECODED ||
                    ret == CEDAR_RESULT_KEYFRAME_DECODED)
                    decoded_total++;
                if (ret == CEDAR_RESULT_CONTINUE || ret == CEDAR_RESULT_OK)
                    continue;
                /* Catch up if an earlier submission was left undecoded. */
                if ((ret == CEDAR_RESULT_FRAME_DECODED ||
                     ret == CEDAR_RESULT_KEYFRAME_DECODED) &&
                    decoded_total < submitted && wait_ms > 0)
                    continue;
                if (ret == CEDAR_RESULT_NO_BITSTREAM &&
                    decoded_total < submitted && wait_ms > 0 &&
                    now_ns() - t_submit < (uint64_t)(wait_ms * 1e6)) {
                    /*
                     * The parser thread splits frames asynchronously;
                     * wait until this submission is decoded instead of
                     * leaving it for the next one.
                     */
                    waits++;
                    if (!kick)
                        usleep(200);
                    tries--;
                    continue;
                }
                break;
            }
            {
                int left = cedar_stream_frames(dec);

                if (left > 0 && (unsigned long)left > sbm_max)
                    sbm_max = (unsigned long)left;
            }
            if (ret < 0) {
                errors++;
                if (verbose || errors < 5)
                    fprintf(stderr, "decode result %d (%s) at au %zu\n", ret,
                            cedar_result_name(ret), i);
            }

            while ((pic = cedar_request_picture(dec)) != NULL) {
                unsigned long idx =
                    (unsigned long)((pic->nPts + 8333) / 16667);

                pictures++;
                if (pic->nPts >= 0 && idx < submitted) {
                    true_ms[ntrue++] =
                        (float)((now_ns() - submit_ns[idx]) / 1e6);
                    lag_last = submitted - 1 - idx;
                    if (lag_last > lag_max)
                        lag_max = lag_last;
                }
                if (marker && !rot_open && pic->pData0 != NULL &&
                    pic->nLineStride > 0) {
                    unsigned v;
                    /* aus[i] was just submitted; loops restart at 0. */
                    int lag = marker_lag((const unsigned char *)pic->pData0,
                                         (unsigned)pic->nLineStride, mark_x,
                                         mark_y, i, &v);

                    mark_lag[lag < 9 ? lag : 9]++;
                    if (verbose)
                        printf("mark: submitted=%lu y=%u lag=%d pts=%lld\n",
                               submitted, v, lag, (long long)pic->nPts);
                } else if (verbose) {
                    printf("pts: submitted=%lu pic_pts=%lld idx=%lu\n",
                           submitted, (long long)pic->nPts, idx);
                }
                if (first_pic_after == 0)
                    first_pic_after = submitted;
                if (pic->bFrameErrorFlag)
                    frame_err++;
                if (pictures == 1 || verbose)
                    dump_picture(pic);
                if (dump_path != NULL && !rot_open && pictures == dump_at) {
                    if (dump_luma(pic, dump_path) == 0)
                        printf("dump: picture %lu luma -> %s\n", pictures,
                               dump_path);
                    else
                        fprintf(stderr, "dump: cannot write %s\n", dump_path);
                }
                if (latest != NULL)
                    cedar_return_picture(dec, latest);
                latest = pic;
            }
            if (latest == NULL) {
                nopic++;
                continue;
            }
            t_now = now_ns();
            lat_ms[nlat++] = (float)((t_now - t_submit) / 1e6);
            if (verbose)
                printf("frame: au=%zu bytes submit->picture=%.2f ms\n",
                       aus[i].len, (t_now - t_submit) / 1e6);

            if (rot_open) {
                struct dispv_frame f;
                struct g2drot_src src;
                uint64_t t0;
                int idx;

                /* Reuse picture_to_frame for crop and plane addresses. */
                if (picture_to_frame(latest, color_space, &disp, &f) != 0) {
                    fprintf(stderr, "picture cannot be rotated\n");
                    cedar_return_picture(dec, latest);
                    g_stop = 1;
                    break;
                }
                memset(&src, 0, sizeof(src));
                src.addr_y = f.addr_y;
                src.addr_c = f.addr_c;
                src.stride = f.stride;
                src.alloc_h = f.alloc_h;
                src.crop_x = f.crop_x;
                src.crop_y = f.crop_y;
                src.crop_w = f.crop_w;
                src.crop_h = f.crop_h;
                src.nv21 = f.format == TSP_DISP_FORMAT_YUV420_SP_VUVU;
                t0 = now_ns();
                idx = g2drot_rotate(&rot, &src, g2d_flag);
                t_now = now_ns();
                /* The blit is synchronous: the picture is free again. */
                cedar_return_picture(dec, latest);
                latest = NULL;
                if (idx < 0) {
                    perror("G2D_CMD_BITBLT_H");
                    g_stop = 1;
                    break;
                }
                rot_ms[nrot++] = (float)((t_now - t0) / 1e6);
                if (marker) {
                    unsigned v;
                    int lag = marker_lag(
                        (const unsigned char *)rot.buf[idx].ion.map, rot.dst_w,
                        mark_x, mark_y, i, &v);

                    mark_lag[lag < 9 ? lag : 9]++;
                }
                if (dump_path != NULL && pictures == dump_at) {
                    if (dump_plane(dump_path,
                                   (const unsigned char *)rot.buf[idx].ion.map,
                                   rot.dst_w, 0, 0, rot.dst_w,
                                   rot.dst_h) == 0)
                        printf("dump: picture %lu rotated luma -> %s\n",
                               pictures, dump_path);
                    else
                        fprintf(stderr, "dump: cannot write %s\n",
                                dump_path);
                }
                lat_ms[nlat - 1] = (float)((t_now - t_submit) / 1e6);
                if (display) {
                    memset(&f, 0, sizeof(f));
                    f.dmabuf_fd = rot.buf[idx].ion.fd;
                    if (rot_v1 && rot.buf[idx].iova != 0) {
                        f.dmabuf_fd = -1;
                        f.addr_y = rot.buf[idx].iova;
                        f.addr_c = rot.buf[idx].iova +
                                   (uint64_t)rot.dst_w * rot.dst_h;
                    }
                    f.format = src.nv21 ? TSP_DISP_FORMAT_YUV420_SP_VUVU
                                        : TSP_DISP_FORMAT_YUV420_SP_UVUV;
                    f.color_space = color_space;
                    f.stride = rot.dst_w;
                    f.alloc_h = rot.dst_h;
                    f.crop_w = rot.dst_w;
                    f.crop_h = rot.dst_h;
                    f.win = dispv_fit(disp.screen_w, disp.screen_h,
                                      rot.dst_w, rot.dst_h);
                    if (shown == 0)
                        printf("frame: rotated fd=%d addr=%#llx %ux%u "
                               "win=%d,%d %ux%u\n", f.dmabuf_fd,
                               (unsigned long long)f.addr_y, f.crop_w,
                               f.crop_h, f.win.x, f.win.y, f.win.width,
                               f.win.height);
                    t0 = now_ns();
                    if (dispv_show(&disp, &f) != 0) {
                        perror(f.dmabuf_fd >= 0 ? "DISP_LAYER_SET_CONFIG2"
                                                : "DISP_LAYER_SET_CONFIG");
                        g_stop = 1;
                        break;
                    }
                    t_now = now_ns();
                    show_ms[nshow++] = (float)((t_now - t0) / 1e6);
                    shown++;
                }
            } else if (display) {
                struct dispv_frame f;
                uint64_t t0;
                int j;

                if (picture_to_frame(latest, color_space, &disp, &f) != 0) {
                    fprintf(stderr, "picture cannot be displayed "
                                    "(fmt=%d phyY=0x%llx)\n",
                            latest->ePixelFormat,
                            (unsigned long long)latest->phyYBufAddr);
                    cedar_return_picture(dec, latest);
                    g_stop = 1;
                    break;
                }
                if (shown == 0)
                    printf("frame: stride=%u alloc_h=%u crop=%u,%u %ux%u "
                           "win=%d,%d %ux%u\n",
                           f.stride, f.alloc_h, f.crop_x, f.crop_y, f.crop_w,
                           f.crop_h, f.win.x, f.win.y, f.win.width,
                           f.win.height);
                t0 = now_ns();
                if (dispv_show(&disp, &f) != 0) {
                    perror(f.dmabuf_fd < 0 ? "DISP_LAYER_SET_CONFIG"
                                           : "DISP_LAYER_SET_CONFIG2");
                    cedar_return_picture(dec, latest);
                    g_stop = 1;
                    break;
                }
                t_now = now_ns();
                show_ms[nshow++] = (float)((t_now - t0) / 1e6);
                shown++;
                for (j = 0; j < nheld; j++)
                    if (held[j].retired_ns == 0)
                        held[j].retired_ns = t_now;
                if (nheld == MAX_HELD) {
                    fprintf(stderr, "held list overflow\n");
                    g_stop = 1;
                    break;
                }
                held[nheld].pic = latest;
                held[nheld].retired_ns = 0;
                nheld++;
                /* Return everything that has been off screen long enough. */
                for (j = 0; j < nheld;) {
                    if (held[j].retired_ns != 0 &&
                        t_now >= held[j].retired_ns +
                                     (uint64_t)(keep_ms * 1e6)) {
                        cedar_return_picture(dec, held[j].pic);
                        held[j] = held[--nheld];
                    } else {
                        j++;
                    }
                }
            } else {
                cedar_return_picture(dec, latest);
            }

            if (t_now - t_last_report >= 5000000000ull) {
                double wall = (t_now - t_start) / 1e9;

                printf("t=%.0fs submitted=%lu pictures=%lu shown=%lu "
                       "cpu=%.1f%% rss=%ldKiB held=%d\n",
                       wall, submitted, pictures, shown,
                       100.0 * (cpu_seconds() - cpu_start) / wall, rss_kib(),
                       nheld);
                fflush(stdout);
                t_last_report = t_now;
            }
        }
    }

    {
        double wall = (now_ns() - t_start) / 1e9;
        double cpu = cpu_seconds() - cpu_start;

        printf("summary: wall=%.2fs submitted=%lu pictures=%lu shown=%lu "
               "fps=%.1f errors=%lu frame_errors=%lu no_frame_buffer=%lu "
               "no_picture=%lu first_picture_after=%lu\n",
               wall, submitted, pictures, shown, pictures / wall, errors,
               frame_err, nobuf, nopic, first_pic_after);
        printf("cpu: %.3fs = %.1f%% of one core; rss %ld -> %ld KiB\n", cpu,
               100.0 * cpu / wall, rss_start, rss_kib());
        printf("decode results: ok=%lu frame=%lu continue=%lu key=%lu "
               "nobuf=%lu nobits=%lu; sbm_max=%lu lag_last=%lu "
               "lag_max=%lu frames\n",
               results[0], results[1], results[2], results[3], results[4],
               results[5], sbm_max, lag_last, lag_max);
        if (wait_ms > 0)
            printf("parser waits: %lu %s\n", waits,
                   kick ? "idle waits" : "sleeps");
        {
            struct cedar_sync_stats st;

            cedar_sync_stats(&st);
            printf("semwait: %s calls=%lu blocked=%lu timeouts=%lu\n",
                   st.vendor ? "vendor" : st.calls ? "interposed"
                                                   : "NOT interposed",
                   st.calls, st.blocked, st.timeouts);
        }
        if (marker)
            printf("marker lag (frames): 0=%lu 1=%lu 2=%lu 3=%lu 4=%lu "
                   "5=%lu 6=%lu 7=%lu 8=%lu 9+=%lu\n", mark_lag[0],
                   mark_lag[1], mark_lag[2], mark_lag[3], mark_lag[4],
                   mark_lag[5], mark_lag[6], mark_lag[7], mark_lag[8],
                   mark_lag[9]);
        report_series("submit->picture", lat_ms, nlat);
        report_series("same-frame latency", true_ms, ntrue);
        report_series("submit->decode call", pre_ms, nhw);
        report_series("decode call", hw_ms, nhw);
        if (rot_open)
            report_series("g2d rotate", rot_ms, nrot);
        if (display)
            report_series("layer ioctl", show_ms, nshow);
    }

    if (display) {
        if (dummy.fd >= 0) {
            uint64_t t0 = now_ns();

            if (dispv_flush_imports(&disp, dummy.fd) != 0)
                perror("flush imports");
            printf("display: flushed imports in %.0f ms\n",
                   (now_ns() - t0) / 1e6);
        } else {
            dispv_hide(&disp);
        }
        /* Let the hardware latch the disable before buffers are reused. */
        usleep(50000);
    }
    /* The layer is off; all pictures can go back now. */
    while (nheld > 0)
        cedar_return_picture(dec, held[--nheld].pic);
    if (rot_open)
        g2drot_unmap_iova(&rot);
    cedar_close(dec);

out:
    alarm(0);
    if (display)
        dispv_close(&disp);
    /* After the layer is off: the display may still read these buffers. */
    if (rot_open) {
        if (display)
            usleep(50000);
        g2drot_close(&rot);
    }
    /* The driver keeps its own reference to the dummy until next session. */
    ionbuf_free(dummy_ion_fd, &dummy);
    if (dummy_ion_fd >= 0)
        close(dummy_ion_fd);
    /* Only now: its release unmaps every Cedar mapping of this process. */
    if (own_cedar_fd >= 0)
        close(own_cedar_fd);
    g_disp_fd = -1;
    free(lat_ms);
    free(show_ms);
    free(true_ms);
    free(submit_ns);
    free(hw_ms);
    free(pre_ms);
    free(rot_ms);
    free(aus);
    free(data);
    return rc;
}
