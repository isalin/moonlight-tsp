/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_VIDEO_CEDAR_H
#define MOONLIGHT_TSP_VIDEO_CEDAR_H

#include <stdint.h>

#include <Limelight.h>

/*
 * moonlight-common-c video renderer: Cedar hardware decode, G2D rotation to
 * the portrait panel, and a display-engine video layer (DE ch0/lyr0 above
 * the framebuffer). Runs entirely in common-c's decoder thread; the GPU and
 * CPU never touch pixels.
 */
extern DECODER_RENDERER_CALLBACKS video_cedar_callbacks;

/* Display-engine position of the video layer. */
#define VIDEO_DISP_CHANNEL 0
#define VIDEO_DISP_LAYER 0
#define VIDEO_DISP_ZORDER 32

/* Interval statistics, reset by each video_cedar_take_stats call. */
struct video_stats {
    unsigned long frames;      /* decode units received */
    unsigned long idr;         /* of which IDR frames */
    unsigned long pictures;    /* pictures out of the decoder */
    unsigned long shown;       /* pictures committed to the layer */
    unsigned long skipped;     /* decoded but not shown (no free buffer) */
    unsigned long hidden;      /* decoded while the layer was hidden */
    unsigned long late;        /* frames whose own picture missed the
                                  parse deadline (an older one shown) */
    unsigned long errors;      /* decode errors and damaged pictures */
    unsigned long need_idr;    /* DR_NEED_IDR returns */
    unsigned long recreated;   /* decoder rebuilds */
    uint64_t bytes;            /* compressed bytes received */
    /* Means in ms over shown pictures. */
    double assemble_ms;        /* first packet to frame complete */
    double queue_ms;           /* frame complete to decoder thread */
    double decode_ms;          /* submit to picture */
    double rotate_ms;          /* G2D blit */
    double commit_ms;          /* layer ioctl */
    double total_ms;           /* first packet to layer commit */
    double total_max_ms;
    double host_ms;            /* encoder-side latency reported by host */
    /* Gaps between the first packets of consecutive frames (stutter). */
    double gap_max_ms;
    unsigned long gaps50;      /* gaps over 50 ms */
    unsigned long gaps100;     /* gaps over 100 ms */
};

void video_cedar_take_stats(struct video_stats *out);

/* Prints one decode trace line for each of the next N frames. */
void video_cedar_set_trace(int frames);

/* Hides the layer (menu on the framebuffer) or shows it again. */
void video_cedar_set_visible(int visible);

/* Fatal-signal path: disables the layer without locking. */
void video_cedar_emergency_off(void);

/*
 * Disables the video layer after flushing the display driver's dmabuf
 * imports, for a dead engine's leftovers (--reset-display).
 */
int video_cedar_reset_display(void);

#endif
