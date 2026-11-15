/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_CEDAR_H
#define MOONLIGHT_TSP_CEDAR_H

#include <stddef.h>
#include <stdint.h>

#include "cedarc_abi.h"

struct cedar_config {
    int codec;          /* CEDAR_CODEC_H264 or CEDAR_CODEC_H265 */
    int width;
    int height;
    int fps;
    int pixel_format;   /* CEDAR_PIXEL_NV12 or CEDAR_PIXEL_NV21 */
    int rotate;         /* raw nRotateDegree index, 0 disables rotation */
    int display_hold;   /* pictures the display may keep */
    int smooth;         /* extra decoder pictures */
    int vbv_bytes;      /* compressed stream buffer */
};

struct cedar_segment {
    const void *data;
    size_t len;
};

struct cedar_decoder;

/* Loads the vendor libraries and registers only the H.264/H.265 plugins. */
int cedar_load(char *err, size_t errlen);

struct cedar_decoder *cedar_open(const struct cedar_config *cfg, char *err,
                                 size_t errlen);
void cedar_close(struct cedar_decoder *dec);

/* Copies one complete access unit into the decoder's stream buffer. */
int cedar_submit(struct cedar_decoder *dec, const struct cedar_segment *seg,
                 int count, int64_t pts);

/* Runs the engine; returns a CEDAR_RESULT_* code. */
int cedar_decode(struct cedar_decoder *dec);

CedarVideoPicture *cedar_request_picture(struct cedar_decoder *dec);
int cedar_return_picture(struct cedar_decoder *dec, CedarVideoPicture *pic);
int cedar_valid_pictures(struct cedar_decoder *dec);
int cedar_stream_frames(struct cedar_decoder *dec);
void cedar_reset(struct cedar_decoder *dec);

const char *cedar_result_name(int result);

#endif
