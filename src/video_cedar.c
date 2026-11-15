/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "video_cedar.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cedar.h"
#include "cedar_sync.h"
#include "disp.h"
#include "g2d_rot.h"
#include "ion.h"
#include "sunxi_g2d_abi.h"

/*
 * Ring of rotated pictures. A buffer is rewritten only after a later
 * buffer replaced it on the layer and two vsyncs (the panel runs at about
 * 60.8 Hz) have passed, so the display engine has stopped reading it.
 * A steady 60 fps stream keeps three buffers busy (on screen plus the two
 * replaced within 34 ms), so four left no room for Wi-Fi bunching: 5.9% of
 * pictures were skipped in a live test. Six leave two spare. A picture that
 * still finds no free buffer is skipped (it is still decoded, so reference
 * frames stay intact).
 */
#define RING_BUFS 6
#define RELEASE_NS 34000000ull

/* How long to wait for the SBM parser to split a submitted frame. */
#define PARSE_WAIT_NS 10000000ull

/* Rebuild the decoder after this many frames without a picture. */
#define STALL_FRAMES 30

#define PTS_STEP 16667

/* Trace entries kept per frame (results of the decode loop). */
#define TRACE_STEPS 12

static const unsigned char aud_h265[] = { 0, 0, 0, 1, 0x46, 0x01, 0x50 };

struct segs {
    struct cedar_segment *v;
    int n, cap;
};

static struct {
    struct cedar_config cfg;
    struct cedar_decoder *dec;
    int cedar_fd;               /* private fd keeping our IOMMU mappings */
    struct g2drot rot;
    int rot_open;
    struct dispv disp;
    int disp_open;
    int ion_fd;
    struct ionbuf dummy;
    uint64_t busy_until[RING_BUFS];
    int on_screen;              /* ring index on the layer, -1 none */
    int color_space;
    unsigned char pps[256];     /* H.264: latest PPS, closes each frame */
    size_t pps_len;
    struct segs segs;
    unsigned long no_picture_run;
    int logged_semwait;
    int pending;                /* submitted frames not yet decoded */
    int trace;                  /* frames left to trace (-v) */

    pthread_mutex_t disp_lock;  /* disp, visible */
    int visible;

    pthread_mutex_t stats_lock;
    struct video_stats st;
    unsigned long n_lat;
    uint64_t last_rx_us;        /* previous frame's first packet */
} V = {
    .cedar_fd = -1,
    .ion_fd = -1,
    .dummy = IONBUF_INIT,
    .on_screen = -1,
    .disp_lock = PTHREAD_MUTEX_INITIALIZER,
    .stats_lock = PTHREAD_MUTEX_INITIALIZER,
    .visible = 1,
};

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void video_cedar_set_trace(int frames)
{
    V.trace = frames;
}

/* Lists the NAL unit types of one buffer, e.g. "35,32,33,34,39,19". */
static void nal_types(const unsigned char *p, int len, int hevc, char *out,
                      size_t cap)
{
    size_t used = 0;
    int i, n = 0;

    out[0] = 0;
    for (i = 0; i + 3 < len && n < 8; i++) {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
            continue;
        used += (size_t)snprintf(out + used, used < cap ? cap - used : 0,
                                 "%s%d", n ? "," : "",
                                 hevc ? (p[i + 3] >> 1) & 0x3f
                                      : p[i + 3] & 0x1f);
        if (used >= cap)
            break;
        n++;
        i += 3;
    }
}

static int map_ring(void)
{
    if (V.cedar_fd < 0)
        V.cedar_fd = open("/dev/cedar_dev", O_RDWR | O_CLOEXEC);
    if (V.cedar_fd < 0)
        return -1;
    return g2drot_map_iova(&V.rot, V.cedar_fd);
}

/* Destroys the decoder; the ring mappings go with it (see g2d_rot.h). */
static void close_decoder(void)
{
    if (V.rot_open)
        g2drot_unmap_iova(&V.rot);
    cedar_close(V.dec);
    V.dec = NULL;
}

static int open_decoder(void)
{
    char err[160];

    V.dec = cedar_open(&V.cfg, err, sizeof(err));
    if (V.dec == NULL) {
        fprintf(stderr, "video: %s\n", err);
        return -1;
    }
    /* Map only while a decoder holds the engine. */
    if (map_ring() != 0) {
        perror("video: map rotation ring");
        close_decoder();
        return -1;
    }
    return 0;
}

static void release_all(void)
{
    if (V.disp_open) {
        pthread_mutex_lock(&V.disp_lock);
        if (V.dummy.fd >= 0)
            dispv_flush_imports(&V.disp, V.dummy.fd);
        else
            dispv_hide(&V.disp);
        dispv_close(&V.disp);
        V.disp_open = 0;
        pthread_mutex_unlock(&V.disp_lock);
    }
    if (V.dec != NULL)
        close_decoder();
    if (V.cedar_fd >= 0) {
        close(V.cedar_fd);
        V.cedar_fd = -1;
    }
    if (V.rot_open) {
        g2drot_close(&V.rot);
        V.rot_open = 0;
    }
    if (V.ion_fd >= 0) {
        ionbuf_free(V.ion_fd, &V.dummy);
        close(V.ion_fd);
        V.ion_fd = -1;
    }
    free(V.segs.v);
    memset(&V.segs, 0, sizeof(V.segs));
}

static int vc_setup(int video_format, int width, int height, int redraw_rate,
                    void *context, int dr_flags)
{
    char err[160];
    int i;

    (void)context;
    (void)dr_flags;
    memset(&V.cfg, 0, sizeof(V.cfg));
    if (video_format & VIDEO_FORMAT_MASK_H264)
        V.cfg.codec = CEDAR_CODEC_H264;
    else if (video_format & VIDEO_FORMAT_MASK_H265)
        V.cfg.codec = CEDAR_CODEC_H265;
    else {
        fprintf(stderr, "video: unsupported format %#x\n", video_format);
        return -1;
    }
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        fprintf(stderr, "video: bad size %dx%d\n", width, height);
        return -1;
    }
    V.cfg.width = width;
    V.cfg.height = height;
    V.cfg.fps = redraw_rate > 0 ? redraw_rate : 60;
    V.cfg.pixel_format = CEDAR_PIXEL_NV21;
    V.cfg.display_hold = 3;
    V.cfg.smooth = 1;
    V.cfg.vbv_bytes = 4 << 20;
    V.pps_len = 0;
    V.on_screen = -1;
    for (i = 0; i < RING_BUFS; i++)
        V.busy_until[i] = 0;
    V.no_picture_run = 0;
    V.logged_semwait = 0;
    V.pending = 0;
    pthread_mutex_lock(&V.stats_lock);
    memset(&V.st, 0, sizeof(V.st));
    V.n_lat = 0;
    pthread_mutex_unlock(&V.stats_lock);

    /* Sunshine encodes limited-range BT.601 unless asked otherwise. */
    V.color_space = TSP_DISP_BT601;

    if (dispv_open(&V.disp, VIDEO_DISP_CHANNEL, VIDEO_DISP_LAYER,
                   VIDEO_DISP_ZORDER) != 0) {
        perror("video: open /dev/disp");
        goto fail;
    }
    V.disp_open = 1;
    V.ion_fd = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (V.ion_fd < 0 || dispv_alloc_dummy(V.ion_fd, &V.dummy) != 0) {
        perror("video: flush dummy");
        goto fail;
    }
    /* A landscape stream turned 90 degrees fills the portrait panel. */
    if (g2drot_open(&V.rot, RING_BUFS, (unsigned)height, (unsigned)width,
                    TSP_ION_HEAP_SYS_USER, 0) != 0) {
        perror("video: rotation ring");
        goto fail;
    }
    V.rot_open = 1;
    if (cedar_load(err, sizeof(err)) != 0) {
        fprintf(stderr, "video: %s\n", err);
        goto fail;
    }
    /*
     * The decoder itself is created by the first decode unit: between
     * creation and the first bitstream a vendor thread polls a semaphore
     * with zero timeout in a tight loop (1.8 million calls and a full core
     * for the second before the first video packet).
     */
    printf("video: %s %dx%d@%d on ch%d/%d, panel %ux%u\n",
           V.cfg.codec == CEDAR_CODEC_H265 ? "hevc" : "h264", width, height,
           V.cfg.fps, VIDEO_DISP_CHANNEL, VIDEO_DISP_LAYER, V.disp.screen_w,
           V.disp.screen_h);
    fflush(stdout);
    return 0;

fail:
    release_all();
    return -1;
}

static void vc_cleanup(void)
{
    release_all();
}

static int segs_add(struct segs *s, const void *data, size_t len)
{
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 16;
        struct cedar_segment *v = realloc(s->v, (size_t)cap * sizeof(*v));

        if (v == NULL)
            return -1;
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n].data = data;
    s->v[s->n].len = len;
    s->n++;
    return 0;
}

/* Fills the G2D source from a decoded picture; 0 on success. */
static int picture_source(const CedarVideoPicture *p, struct g2drot_src *s)
{
    unsigned stride = (unsigned)p->nLineStride;

    memset(s, 0, sizeof(*s));
    if (p->ePixelFormat != CEDAR_PIXEL_NV21 &&
        p->ePixelFormat != CEDAR_PIXEL_NV12)
        return -1;
    if (p->phyYBufAddr == 0 || p->phyCBufAddr <= p->phyYBufAddr ||
        stride == 0)
        return -1;
    s->addr_y = p->phyYBufAddr;
    s->addr_c = p->phyCBufAddr;
    s->stride = stride;
    s->alloc_h = (unsigned)((p->phyCBufAddr - p->phyYBufAddr) / stride);
    if (p->nRightOffset > p->nLeftOffset &&
        p->nBottomOffset > p->nTopOffset) {
        s->crop_x = (unsigned)p->nLeftOffset;
        s->crop_y = (unsigned)p->nTopOffset;
        s->crop_w = (unsigned)(p->nRightOffset - p->nLeftOffset);
        s->crop_h = (unsigned)(p->nBottomOffset - p->nTopOffset);
    } else {
        s->crop_w = (unsigned)p->nWidth;
        s->crop_h = (unsigned)p->nHeight;
    }
    s->nv21 = p->ePixelFormat == CEDAR_PIXEL_NV21;
    return 0;
}

/* Picks a ring buffer the display has let go of, or -1. */
static int free_slot(uint64_t t)
{
    int i, best = -1;

    for (i = 0; i < RING_BUFS; i++) {
        if (i == V.on_screen || t < V.busy_until[i])
            continue;
        if (best < 0 || V.busy_until[i] < V.busy_until[best])
            best = i;
    }
    return best;
}

/*
 * Rotates the picture into a free ring buffer and commits it to the layer.
 * Returns 1 if shown, 0 if skipped, -1 on a hardware error. The picture is
 * no longer needed when this returns (the blit is synchronous).
 */
static int present(CedarVideoPicture *pic, double *rot_ms, double *commit_ms)
{
    struct g2drot_src src;
    struct dispv_frame f;
    uint64_t t0, t1, t2;
    int slot, idx, ret = 0;

    if (picture_source(pic, &src) != 0) {
        fprintf(stderr, "video: picture cannot be rotated (fmt=%d)\n",
                pic->ePixelFormat);
        return -1;
    }
    pthread_mutex_lock(&V.disp_lock);
    if (!V.visible || !V.disp_open)
        goto out;
    t0 = now_ns();
    slot = free_slot(t0);
    if (slot < 0)
        goto out;
    V.rot.next = (unsigned)slot;
    idx = g2drot_rotate(&V.rot, &src, TSP_G2D_ROT_270);
    if (idx < 0) {
        perror("video: G2D rotate");
        ret = -1;
        goto out;
    }
    t1 = now_ns();

    memset(&f, 0, sizeof(f));
    f.dmabuf_fd = V.rot.buf[idx].ion.fd;
    f.format = src.nv21 ? TSP_DISP_FORMAT_YUV420_SP_VUVU
                        : TSP_DISP_FORMAT_YUV420_SP_UVUV;
    f.color_space = V.color_space;
    f.stride = V.rot.dst_w;
    f.alloc_h = V.rot.dst_h;
    f.crop_w = V.rot.dst_w;
    f.crop_h = V.rot.dst_h;
    f.win = dispv_fit(V.disp.screen_w, V.disp.screen_h, V.rot.dst_w,
                      V.rot.dst_h);
    if (dispv_show(&V.disp, &f) != 0) {
        perror("video: DISP_LAYER_SET_CONFIG2");
        ret = -1;
        goto out;
    }
    t2 = now_ns();
    if (V.on_screen >= 0)
        V.busy_until[V.on_screen] = t2 + RELEASE_NS;
    V.on_screen = idx;
    *rot_ms = (t1 - t0) / 1e6;
    *commit_ms = (t2 - t1) / 1e6;
    ret = 1;
out:
    pthread_mutex_unlock(&V.disp_lock);
    return ret;
}

static int rebuild_decoder(void)
{
    pthread_mutex_lock(&V.disp_lock);
    /* The layer keeps its own import of the ring; only Cedar restarts. */
    close_decoder();
    pthread_mutex_unlock(&V.disp_lock);
    pthread_mutex_lock(&V.stats_lock);
    V.st.recreated++;
    pthread_mutex_unlock(&V.stats_lock);
    fprintf(stderr, "video: rebuilding decoder\n");
    V.pending = 0;
    return open_decoder();
}

static int vc_submit(PDECODE_UNIT du)
{
    uint64_t t_enter_us = LiGetMicroseconds();
    uint64_t t_submit, deadline;
    CedarVideoPicture *pic, *latest = NULL;
    PLENTRY e;
    int64_t pts = (int64_t)du->frameNumber * PTS_STEP;
    int64_t latest_pts = -1;
    unsigned seq;
    int ret = CEDAR_RESULT_OK, tries, parsed = 0, errors = 0, shown = 0;
    int late = 0, pending0, npics = 0, nsteps = 0;
    double rot_ms = 0, commit_ms = 0, decode_ms = 0;
    struct cedar_sync_stats ss0;
    struct { int r; double ms; } steps[TRACE_STEPS];
    char nals[160];

    if (V.dec == NULL && open_decoder() != 0)
        return DR_NEED_IDR;
    if (V.trace > 0) {
        size_t used = 0;

        cedar_sync_stats(&ss0);
        nals[0] = 0;
        for (e = du->bufferList; e != NULL && used < sizeof(nals); e = e->next) {
            char t[64];

            nal_types((const unsigned char *)e->data, e->length,
                      V.cfg.codec == CEDAR_CODEC_H265, t, sizeof(t));
            used += (size_t)snprintf(nals + used, sizeof(nals) - used,
                                     "%s%d:%s", used ? " " : "",
                                     e->bufferType, t);
        }
    }

    V.segs.n = 0;
    for (e = du->bufferList; e != NULL; e = e->next) {
        if (e->bufferType == BUFFER_TYPE_PPS && V.cfg.codec ==
                CEDAR_CODEC_H264 && (size_t)e->length <= sizeof(V.pps)) {
            memcpy(V.pps, e->data, (size_t)e->length);
            V.pps_len = (size_t)e->length;
        }
        if (segs_add(&V.segs, e->data, (size_t)e->length) != 0)
            return DR_NEED_IDR;
    }
    /*
     * The SBM parser closes a frame only when the next one starts. A
     * trailing PPS (H.264) or AUD (HEVC) closes it at once.
     */
    if (V.cfg.codec == CEDAR_CODEC_H265)
        segs_add(&V.segs, aud_h265, sizeof(aud_h265));
    else if (V.pps_len > 0)
        segs_add(&V.segs, V.pps, V.pps_len);

    pending0 = V.pending;
    seq = cedar_sync_seq();
    t_submit = now_ns();
    if (cedar_submit(V.dec, V.segs.v, V.segs.n, pts) != 0) {
        /* Stream buffer full or broken: start over from an IDR frame. */
        fprintf(stderr, "video: submit failed (%d bytes)\n", du->fullLength);
        cedar_reset(V.dec);
        V.pending = 0;
        errors++;
        goto done;
    }
    /*
     * Picture PTS values are rewritten by the decoder, so they cannot say
     * which frame a picture came from. Count instead: every decoded result
     * finishes the oldest submitted frame, and ours is done when none is
     * left. A frame the decoder dropped would leave the count high, so it
     * is capped below.
     */
    if (V.pending < 2)
        V.pending++;
    deadline = t_submit + PARSE_WAIT_NS;

    for (tries = 0; tries < 32; tries++) {
        if (!parsed) {
            /* Sleep until the parser has consumed the submission. */
            parsed = cedar_sync_wait(seq, deadline);
            seq = cedar_sync_seq();
        }
        ret = cedar_decode(V.dec);
        if (nsteps < TRACE_STEPS) {
            steps[nsteps].r = ret;
            steps[nsteps].ms = (now_ns() - t_submit) / 1e6;
            nsteps++;
        }
        if (ret == CEDAR_RESULT_FRAME_DECODED ||
            ret == CEDAR_RESULT_KEYFRAME_DECODED) {
            if (V.pending > 0)
                V.pending--;
            while ((pic = cedar_request_picture(V.dec)) != NULL) {
                if (pic->bFrameErrorFlag)
                    errors++;
                if (latest != NULL)
                    cedar_return_picture(V.dec, latest);
                latest = pic;
                latest_pts = pic->nPts;
                npics++;
            }
            if (V.pending == 0)
                break;
            continue;   /* an older frame: keep going for ours */
        }
        if (ret == CEDAR_RESULT_NO_BITSTREAM) {
            if (now_ns() < deadline) {
                parsed = 0;
                continue;
            }
            break;
        }
        if (ret == CEDAR_RESULT_CONTINUE || ret == CEDAR_RESULT_OK)
            continue;
        if (ret == CEDAR_RESULT_NO_FRAME_BUFFER) {
            /* We hold no pictures, so this is transient. */
            usleep(1000);
            continue;
        }
        errors++;
        break;
    }
    late = V.pending > 0;
    if (V.trace > 0) {
        struct cedar_sync_stats ss1;
        char st[160];
        size_t used = 0;
        int i;

        cedar_sync_stats(&ss1);
        st[0] = 0;
        for (i = 0; i < nsteps && used < sizeof(st); i++)
            used += (size_t)snprintf(st + used, sizeof(st) - used,
                                     "%s%d@%.2f", i ? "," : "", steps[i].r,
                                     steps[i].ms);
        printf("trace f=%d %s len=%d [%s] pend %d->%d steps=%s pics=%d "
               "pts=%lld/%lld semcalls+%lu blocked+%lu\n",
               du->frameNumber, du->frameType == FRAME_TYPE_IDR ? "IDR" : "P",
               du->fullLength, nals, pending0, V.pending, st, npics,
               (long long)latest_pts, (long long)pts,
               ss1.calls - ss0.calls, ss1.blocked - ss0.blocked);
        fflush(stdout);
        V.trace--;
    }
    if (latest != NULL) {
        int r;

        decode_ms = (now_ns() - t_submit) / 1e6;
        r = present(latest, &rot_ms, &commit_ms);
        cedar_return_picture(V.dec, latest);
        if (r < 0)
            errors++;
        shown = r > 0;
        V.no_picture_run = 0;
    } else {
        V.no_picture_run++;
    }

done:
    if (!V.logged_semwait && du->frameNumber > 120) {
        struct cedar_sync_stats ss;

        cedar_sync_stats(&ss);
        printf("semwait: %s calls=%lu blocked=%lu timeouts=%lu\n",
               ss.vendor ? "vendor" : ss.calls ? "interposed"
                                               : "NOT interposed",
               ss.calls, ss.blocked, ss.timeouts);
        fflush(stdout);
        V.logged_semwait = 1;
    }

    pthread_mutex_lock(&V.stats_lock);
    V.st.frames++;
    if (du->frameType == FRAME_TYPE_IDR)
        V.st.idr++;
    V.st.bytes += (uint64_t)du->fullLength;
    if (latest != NULL)
        V.st.pictures++;
    V.st.late += (unsigned long)late;
    V.st.errors += (unsigned long)errors;
    if (latest != NULL && !shown) {
        if (V.visible)
            V.st.skipped++;
        else
            V.st.hidden++;
    }
    if (V.last_rx_us != 0 && du->receiveTimeUs > V.last_rx_us) {
        double gap = (du->receiveTimeUs - V.last_rx_us) / 1e3;

        if (gap > V.st.gap_max_ms)
            V.st.gap_max_ms = gap;
        V.st.gaps50 += gap > 50.0;
        V.st.gaps100 += gap > 100.0;
    }
    V.last_rx_us = du->receiveTimeUs;
    if (shown) {
        uint64_t t_done = LiGetMicroseconds();
        double total = (t_done - du->receiveTimeUs) / 1e3;

        V.st.shown++;
        V.n_lat++;
        V.st.assemble_ms += (du->enqueueTimeUs - du->receiveTimeUs) / 1e3;
        V.st.queue_ms += (t_enter_us - du->enqueueTimeUs) / 1e3;
        V.st.decode_ms += decode_ms;
        V.st.rotate_ms += rot_ms;
        V.st.commit_ms += commit_ms;
        V.st.total_ms += total;
        if (total > V.st.total_max_ms)
            V.st.total_max_ms = total;
        V.st.host_ms += du->frameHostProcessingLatency / 10.0;
    }
    pthread_mutex_unlock(&V.stats_lock);

    if (ret == CEDAR_RESULT_RESOLUTION_CHANGE ||
        V.no_picture_run >= STALL_FRAMES) {
        V.no_picture_run = 0;
        if (rebuild_decoder() != 0)
            fprintf(stderr, "video: decoder rebuild failed\n");
        errors++;
    }
    if (errors > 0) {
        pthread_mutex_lock(&V.stats_lock);
        V.st.need_idr++;
        pthread_mutex_unlock(&V.stats_lock);
        return DR_NEED_IDR;
    }
    return DR_OK;
}

void video_cedar_take_stats(struct video_stats *out)
{
    pthread_mutex_lock(&V.stats_lock);
    *out = V.st;
    if (V.n_lat > 0) {
        out->assemble_ms /= V.n_lat;
        out->queue_ms /= V.n_lat;
        out->decode_ms /= V.n_lat;
        out->rotate_ms /= V.n_lat;
        out->commit_ms /= V.n_lat;
        out->total_ms /= V.n_lat;
        out->host_ms /= V.n_lat;
    }
    memset(&V.st, 0, sizeof(V.st));
    V.n_lat = 0;
    pthread_mutex_unlock(&V.stats_lock);
}

void video_cedar_set_visible(int visible)
{
    pthread_mutex_lock(&V.disp_lock);
    V.visible = visible != 0;
    if (!V.visible && V.disp_open && V.disp.enabled) {
        dispv_hide(&V.disp);
        /* The hidden picture's buffer is free after the next vsyncs. */
        if (V.on_screen >= 0)
            V.busy_until[V.on_screen] = now_ns() + RELEASE_NS;
        V.on_screen = -1;
    }
    pthread_mutex_unlock(&V.disp_lock);
}

void video_cedar_emergency_off(void)
{
    /* Signal context: no locks, one ioctl on the already-open fd. */
    if (V.disp_open && V.disp.fd >= 0)
        dispv_disable_layer(V.disp.fd, V.disp.screen, VIDEO_DISP_CHANNEL,
                            VIDEO_DISP_LAYER);
}

int video_cedar_reset_display(void)
{
    return dispv_reset(VIDEO_DISP_CHANNEL, VIDEO_DISP_LAYER,
                       VIDEO_DISP_ZORDER, 1);
}

DECODER_RENDERER_CALLBACKS video_cedar_callbacks = {
    .setup = vc_setup,
    .cleanup = vc_cleanup,
    .submitDecodeUnit = vc_submit,
    /*
     * No CAPABILITY_DIRECT_SUBMIT: common-c's bounded queue and decoder
     * thread keep the network receive thread from ever waiting on the
     * hardware, and an overflow there requests an IDR frame by itself.
     */
    .capabilities = 0,
};
