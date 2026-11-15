/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cedar.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cedar_api {
    int loaded;
    void *vdecoder;
    void *videoengine;
    cedar_create_fn create;
    cedar_destroy_fn destroy;
    cedar_initialize_fn initialize;
    cedar_reset_fn reset;
    cedar_decode_fn decode;
    cedar_request_stream_fn request_stream;
    cedar_submit_stream_fn submit_stream;
    cedar_request_picture_fn request_picture;
    cedar_return_picture_fn return_picture;
    cedar_count_fn valid_pictures;
    cedar_count_fn stream_frames;
    cedar_add_plugin_fn add_plugin;
};

struct cedar_decoder {
    CedarDecoder *handle;
    struct cedar_config cfg;
};

static struct cedar_api api;

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || errlen == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static void *open_lib(const char *name)
{
    char path[128];
    void *h;

    snprintf(path, sizeof(path), "/usr/lib/%s", name);
    h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (h == NULL)
        h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    return h;
}

#define LOAD_SYM(handle, field, name)                                        \
    do {                                                                     \
        *(void **)(&api.field) = dlsym((handle), (name));                    \
        if (api.field == NULL) {                                             \
            set_err(err, errlen, "missing symbol %s", (name));               \
            return -1;                                                       \
        }                                                                    \
    } while (0)

int cedar_load(char *err, size_t errlen)
{
    if (api.loaded)
        return 0;

    /*
     * RTLD_GLOBAL makes libvdecoder and its dependencies (libcdc_base,
     * libvideoengine, libVE, libMemAdapter) visible to the codec plugins,
     * which have no DT_NEEDED entries of their own.
     */
    api.vdecoder = open_lib("libvdecoder.so");
    if (api.vdecoder == NULL) {
        set_err(err, errlen, "dlopen libvdecoder.so: %s", dlerror());
        return -1;
    }
    api.videoengine = open_lib("libvideoengine.so");
    if (api.videoengine == NULL) {
        set_err(err, errlen, "dlopen libvideoengine.so: %s", dlerror());
        return -1;
    }

    LOAD_SYM(api.vdecoder, create, "CreateVideoDecoder");
    LOAD_SYM(api.vdecoder, destroy, "DestroyVideoDecoder");
    LOAD_SYM(api.vdecoder, initialize, "InitializeVideoDecoder");
    LOAD_SYM(api.vdecoder, reset, "ResetVideoDecoder");
    LOAD_SYM(api.vdecoder, decode, "DecodeVideoStream");
    LOAD_SYM(api.vdecoder, request_stream, "RequestVideoStreamBuffer");
    LOAD_SYM(api.vdecoder, submit_stream, "SubmitVideoStreamData");
    LOAD_SYM(api.vdecoder, request_picture, "RequestPicture");
    LOAD_SYM(api.vdecoder, return_picture, "ReturnPicture");
    LOAD_SYM(api.vdecoder, valid_pictures, "ValidPictureNum");
    LOAD_SYM(api.vdecoder, stream_frames, "VideoStreamFrameNum");
    LOAD_SYM(api.videoengine, add_plugin, "AddVDPluginSingle");

    /*
     * AddVDPlugin() would dlopen every lib*aw*.so in /usr/lib, including
     * all audio decoders. Register just the two codecs Moonlight uses.
     */
    api.add_plugin((char *)"/usr/lib/libawh264.so");
    api.add_plugin((char *)"/usr/lib/libawh265.so");
    api.loaded = 1;
    return 0;
}

struct cedar_decoder *cedar_open(const struct cedar_config *cfg, char *err,
                                 size_t errlen)
{
    struct cedar_decoder *dec;
    CedarVideoStreamInfo info;
    CedarVConfig vc;
    int ret;

    if (!api.loaded && cedar_load(err, errlen) != 0)
        return NULL;

    dec = calloc(1, sizeof(*dec));
    if (dec == NULL) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    dec->cfg = *cfg;

    dec->handle = api.create();
    if (dec->handle == NULL) {
        set_err(err, errlen, "CreateVideoDecoder failed");
        free(dec);
        return NULL;
    }

    memset(&info, 0, sizeof(info));
    info.eCodecFormat = cfg->codec;
    info.nWidth = cfg->width;
    info.nHeight = cfg->height;
    info.nFrameRate = cfg->fps * 1000;
    info.nFrameDuration = cfg->fps > 0 ? 1000000 / cfg->fps : 0;
    info.bIsFramePackage = 1;

    memset(&vc, 0, sizeof(vc));
    vc.eOutputPixelFormat = cfg->pixel_format;
    vc.bNoBFrames = 1;
    vc.bDisable3D = 1;
    vc.nVbvBufferSize = cfg->vbv_bytes > 0 ? cfg->vbv_bytes : 4 << 20;
    vc.nAlignStride = 16;
    vc.nDeInterlaceHoldingFrameBufferNum = 0;
    vc.nDisplayHoldingFrameBufferNum =
        cfg->display_hold > 0 ? cfg->display_hold : 3;
    vc.nDecodeSmoothFrameBufferNum = cfg->smooth >= 0 ? cfg->smooth : 1;
    if (cfg->rotate != 0) {
        vc.bRotationEn = 1;
        vc.nRotateDegree = cfg->rotate;
        vc.nRotateHoldingFrameBufferNum = vc.nDisplayHoldingFrameBufferNum;
    }

    ret = api.initialize(dec->handle, &info, &vc);
    if (ret != 0) {
        /* InitializeVideoDecoder is not transactional: start over. */
        api.destroy(dec->handle);
        free(dec);
        set_err(err, errlen, "InitializeVideoDecoder failed (%d)", ret);
        return NULL;
    }
    return dec;
}

void cedar_close(struct cedar_decoder *dec)
{
    if (dec == NULL)
        return;
    if (dec->handle != NULL)
        api.destroy(dec->handle);
    free(dec);
}

int cedar_submit(struct cedar_decoder *dec, const struct cedar_segment *seg,
                 int count, int64_t pts)
{
    CedarVideoStreamDataInfo di;
    char *buf = NULL, *ring = NULL;
    int buf_size = 0, ring_size = 0;
    size_t total = 0, copied = 0;
    int i;

    for (i = 0; i < count; i++)
        total += seg[i].len;
    if (total == 0 || total > (size_t)0x7fffffff)
        return -1;

    if (api.request_stream(dec->handle, (int)total, &buf, &buf_size, &ring,
                           &ring_size, 0) != 0)
        return -2;
    if (buf == NULL || (size_t)buf_size + (size_t)ring_size < total)
        return -3;

    for (i = 0; i < count; i++) {
        const unsigned char *src = seg[i].data;
        size_t left = seg[i].len;

        while (left > 0) {
            size_t n;
            char *dst;

            if (copied < (size_t)buf_size) {
                n = (size_t)buf_size - copied;
                dst = buf + copied;
            } else {
                n = total - copied;
                dst = ring + (copied - (size_t)buf_size);
            }
            if (n > left)
                n = left;
            memcpy(dst, src, n);
            src += n;
            left -= n;
            copied += n;
        }
    }

    memset(&di, 0, sizeof(di));
    di.pData = buf;
    di.nLength = (int)total;
    di.nPts = pts;
    di.nPcr = -1;
    di.bIsFirstPart = 1;
    di.bIsLastPart = 1;
    di.bValid = 1;
    di.nStreamIndex = 0;
    if (api.submit_stream(dec->handle, &di, 0) != 0)
        return -4;
    return 0;
}

int cedar_decode(struct cedar_decoder *dec)
{
    return api.decode(dec->handle, 0, 0, 0, 0);
}

CedarVideoPicture *cedar_request_picture(struct cedar_decoder *dec)
{
    return api.request_picture(dec->handle, 0);
}

int cedar_return_picture(struct cedar_decoder *dec, CedarVideoPicture *pic)
{
    return api.return_picture(dec->handle, pic);
}

int cedar_valid_pictures(struct cedar_decoder *dec)
{
    return api.valid_pictures(dec->handle, 0);
}

int cedar_stream_frames(struct cedar_decoder *dec)
{
    return api.stream_frames(dec->handle, 0);
}

void cedar_reset(struct cedar_decoder *dec)
{
    api.reset(dec->handle);
}

const char *cedar_result_name(int result)
{
    switch (result) {
    case CEDAR_RESULT_UNSUPPORTED: return "unsupported";
    case CEDAR_RESULT_OK: return "ok";
    case CEDAR_RESULT_FRAME_DECODED: return "frame";
    case CEDAR_RESULT_CONTINUE: return "continue";
    case CEDAR_RESULT_KEYFRAME_DECODED: return "keyframe";
    case CEDAR_RESULT_NO_FRAME_BUFFER: return "no-frame-buffer";
    case CEDAR_RESULT_NO_BITSTREAM: return "no-bitstream";
    case CEDAR_RESULT_RESOLUTION_CHANGE: return "resolution-change";
    default: return "error";
    }
}
