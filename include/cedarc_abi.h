/*
 * Minimal ABI declarations for the TrimUI Smart Pro vendor CedarC-v1.3.0
 * video decoder (/usr/lib/libvdecoder.so and friends).
 *
 * Layouts were taken from the DWARF debug information shipped inside the
 * device's own libvdecoder.so (see scripts/check-cedarc-abi.sh). Only the
 * fields this program touches are named; the rest are reserved padding.
 * The vendor libraries are loaded at run time with dlopen(); nothing here
 * is linked against them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MOONLIGHT_TSP_CEDARC_ABI_H
#define MOONLIGHT_TSP_CEDARC_ABI_H

#include <stddef.h>
#include <stdint.h>

enum {
    CEDAR_CODEC_H264 = 0x115,
    CEDAR_CODEC_H265 = 0x116,
};

enum {
    CEDAR_PIXEL_DEFAULT = 0x00,
    CEDAR_PIXEL_YUV_PLANER_420 = 0x01,
    CEDAR_PIXEL_YV12 = 0x04,
    CEDAR_PIXEL_NV21 = 0x05,
    CEDAR_PIXEL_NV12 = 0x06,
    CEDAR_PIXEL_YUV_MB32_420 = 0x07,
};

enum {
    CEDAR_RESULT_UNSUPPORTED = -1,
    CEDAR_RESULT_OK = 0,
    CEDAR_RESULT_FRAME_DECODED = 1,
    CEDAR_RESULT_CONTINUE = 2,
    CEDAR_RESULT_KEYFRAME_DECODED = 3,
    CEDAR_RESULT_NO_FRAME_BUFFER = 4,
    CEDAR_RESULT_NO_BITSTREAM = 5,
    CEDAR_RESULT_RESOLUTION_CHANGE = 6,
};

typedef struct CedarVConfig {
    int32_t bScaleDownEn;                      /* 0x00 */
    int32_t bRotationEn;                       /* 0x04 */
    int32_t bSecOutputEn;                      /* 0x08 */
    int32_t nHorizonScaleDownRatio;            /* 0x0c */
    int32_t nVerticalScaleDownRatio;           /* 0x10 */
    int32_t nSDWidth;                          /* 0x14 */
    int32_t nSDHeight;                         /* 0x18 */
    int32_t bAnySizeSD;                        /* 0x1c */
    int32_t nSecHorizonScaleDownRatio;         /* 0x20 */
    int32_t nSecVerticalScaleDownRatio;        /* 0x24 */
    int32_t nRotateDegree;                     /* 0x28 */
    int32_t bThumbnailMode;                    /* 0x2c */
    int32_t eOutputPixelFormat;                /* 0x30 */
    int32_t eSecOutputPixelFormat;             /* 0x34 */
    int32_t bNoBFrames;                        /* 0x38 */
    int32_t bDisable3D;                        /* 0x3c */
    int32_t bSupportMaf;                       /* 0x40 */
    int32_t bDispErrorFrame;                   /* 0x44 */
    int32_t nVbvBufferSize;                    /* 0x48 */
    int32_t nFrameBufferNum;                   /* 0x4c */
    int32_t bSecureosEn;                       /* 0x50 */
    int32_t bGpuBufValid;                      /* 0x54 */
    int32_t nAlignStride;                      /* 0x58 */
    int32_t bIsSoftDecoderFlag;                /* 0x5c */
    int32_t bVirMallocSbm;                     /* 0x60 */
    int32_t bSupportPallocBufBeforeDecode;     /* 0x64 */
    int32_t nDeInterlaceHoldingFrameBufferNum; /* 0x68 */
    int32_t nDisplayHoldingFrameBufferNum;     /* 0x6c */
    int32_t nRotateHoldingFrameBufferNum;      /* 0x70 */
    int32_t nDecodeSmoothFrameBufferNum;       /* 0x74 */
    int32_t bIsTvStream;                       /* 0x78 */
    int32_t nLbcLossyComMod;                   /* 0x7c */
    uint32_t bIsLossy;                         /* 0x80 */
    uint32_t bRcEn;                            /* 0x84 */
    void *memops;                              /* 0x88 */
    int32_t eCtlAfbcMode;                      /* 0x90 */
    int32_t eCtlIptvMode;                      /* 0x94 */
    void *veOpsS;                              /* 0x98 */
    void *pVeOpsSelf;                          /* 0xa0 */
    int32_t bConvertVp910bitTo8bit;            /* 0xa8 */
    uint32_t nVeFreq;                          /* 0xac */
    int32_t bCalledByOmxFlag;                  /* 0xb0 */
    int32_t bSetProcInfoEnable;                /* 0xb4 */
    int32_t nSetProcInfoFreq;                  /* 0xb8 */
    int32_t nChannelNum;                       /* 0xbc */
    int32_t nSupportMaxWidth;                  /* 0xc0 */
    int32_t nSupportMaxHeight;                 /* 0xc4 */
    int32_t commonConfigFlag;                  /* 0xc8 */
    int32_t bATMFlag;                          /* 0xcc */
} CedarVConfig;

typedef struct CedarVideoStreamInfo {
    int32_t eCodecFormat;            /* 0x00 */
    int32_t nWidth;                  /* 0x04 */
    int32_t nHeight;                 /* 0x08 */
    int32_t nFrameRate;              /* 0x0c */
    int32_t nFrameDuration;          /* 0x10 */
    int32_t nAspectRatio;            /* 0x14 */
    int32_t bIs3DStream;             /* 0x18 */
    int32_t nCodecSpecificDataLen;   /* 0x1c */
    char *pCodecSpecificData;        /* 0x20 */
    int32_t bSecureStreamFlag;       /* 0x28 */
    int32_t bSecureStreamFlagLevel1; /* 0x2c */
    int32_t bIsFramePackage;         /* 0x30 */
    int32_t h265ReferencePictureNum; /* 0x34 */
    int32_t bReOpenEngine;           /* 0x38 */
    int32_t bIsFrameCtsTestFlag;     /* 0x3c */
} CedarVideoStreamInfo;

typedef struct CedarVideoStreamDataInfo {
    char *pData;           /* 0x00 */
    int32_t nLength;       /* 0x08 */
    int32_t pad0;
    int64_t nPts;          /* 0x10 */
    int64_t nPcr;          /* 0x18 */
    int32_t bIsFirstPart;  /* 0x20 */
    int32_t bIsLastPart;   /* 0x24 */
    int32_t nID;           /* 0x28 */
    int32_t nStreamIndex;  /* 0x2c */
    int32_t bValid;        /* 0x30 */
    uint32_t bVideoInfoFlag; /* 0x34 */
    void *pVideoInfo;      /* 0x38 */
} CedarVideoStreamDataInfo;

typedef struct CedarVideoPicture {
    int32_t nID;            /* 0x00 */
    int32_t nStreamIndex;   /* 0x04 */
    int32_t ePixelFormat;   /* 0x08 */
    int32_t nWidth;         /* 0x0c */
    int32_t nHeight;        /* 0x10 */
    int32_t nLineStride;    /* 0x14 */
    int32_t nTopOffset;     /* 0x18 */
    int32_t nLeftOffset;    /* 0x1c */
    int32_t nBottomOffset;  /* 0x20 */
    int32_t nRightOffset;   /* 0x24 */
    int32_t nFrameRate;     /* 0x28 */
    int32_t nAspectRatio;   /* 0x2c */
    int32_t bIsProgressive; /* 0x30 */
    int32_t bTopFieldFirst; /* 0x34 */
    int32_t bRepeatTopField; /* 0x38 */
    int32_t pad0;
    int64_t nPts;           /* 0x40 */
    int64_t nPcr;           /* 0x48 */
    char *pData0;           /* 0x50 */
    char *pData1;           /* 0x58 */
    char *pData2;           /* 0x60 */
    char *pData3;           /* 0x68 */
    int32_t bMafValid;      /* 0x70 */
    int32_t pad1;
    char *pMafData;         /* 0x78 */
    int32_t nMafFlagStride; /* 0x80 */
    int32_t bPreFrmValid;   /* 0x84 */
    int32_t nBufId;         /* 0x88 */
    int32_t pad2;
    uint64_t phyYBufAddr;   /* 0x90 */
    uint64_t phyCBufAddr;   /* 0x98 */
    void *pPrivate;         /* 0xa0 */
    int32_t nBufFd;         /* 0xa8 */
    int32_t nBufStatus;     /* 0xac */
    int32_t bTopFieldError; /* 0xb0 */
    int32_t bBottomFieldError; /* 0xb4 */
    int32_t nColorPrimary;  /* 0xb8 */
    int32_t bFrameErrorFlag; /* 0xbc */
    void *pMetaData;        /* 0xc0 */
    int32_t video_full_range_flag;    /* 0xc8 */
    int32_t transfer_characteristics; /* 0xcc */
    int32_t matrix_coeffs;  /* 0xd0 */
    uint8_t colour_primaries; /* 0xd4 */
    uint8_t pad3[3];
    int32_t nLower2BitBufSize;   /* 0xd8 */
    int32_t nLower2BitBufOffset; /* 0xdc */
    int32_t nLower2BitBufStride; /* 0xe0 */
    int32_t b10BitPicFlag;  /* 0xe4 */
    int32_t bEnableAfbcFlag; /* 0xe8 */
    int32_t nLbcLossyComMod; /* 0xec */
    uint32_t bIsLossy;      /* 0xf0 */
    uint32_t bRcEn;         /* 0xf4 */
    int32_t nBufSize;       /* 0xf8 */
    int32_t nAfbcSize;      /* 0xfc */
    int32_t nLbcSize;       /* 0x100 */
    int32_t nDebugCount;    /* 0x104 */
    uint8_t nCurFrameInfo[72]; /* 0x108 */
} CedarVideoPicture;

#define CEDAR_ASSERT_OFF(type, field, off) \
    _Static_assert(offsetof(type, field) == (off), #type "." #field)

CEDAR_ASSERT_OFF(CedarVConfig, bRotationEn, 0x04);
CEDAR_ASSERT_OFF(CedarVConfig, nRotateDegree, 0x28);
CEDAR_ASSERT_OFF(CedarVConfig, eOutputPixelFormat, 0x30);
CEDAR_ASSERT_OFF(CedarVConfig, bNoBFrames, 0x38);
CEDAR_ASSERT_OFF(CedarVConfig, nVbvBufferSize, 0x48);
CEDAR_ASSERT_OFF(CedarVConfig, nFrameBufferNum, 0x4c);
CEDAR_ASSERT_OFF(CedarVConfig, nAlignStride, 0x58);
CEDAR_ASSERT_OFF(CedarVConfig, nDisplayHoldingFrameBufferNum, 0x6c);
CEDAR_ASSERT_OFF(CedarVConfig, nRotateHoldingFrameBufferNum, 0x70);
CEDAR_ASSERT_OFF(CedarVConfig, nDecodeSmoothFrameBufferNum, 0x74);
CEDAR_ASSERT_OFF(CedarVConfig, memops, 0x88);
CEDAR_ASSERT_OFF(CedarVConfig, veOpsS, 0x98);
CEDAR_ASSERT_OFF(CedarVConfig, pVeOpsSelf, 0xa0);
CEDAR_ASSERT_OFF(CedarVConfig, nVeFreq, 0xac);
CEDAR_ASSERT_OFF(CedarVConfig, bATMFlag, 0xcc);
_Static_assert(sizeof(CedarVConfig) == 0xd0, "VConfig size");

CEDAR_ASSERT_OFF(CedarVideoStreamInfo, pCodecSpecificData, 0x20);
CEDAR_ASSERT_OFF(CedarVideoStreamInfo, bIsFramePackage, 0x30);
CEDAR_ASSERT_OFF(CedarVideoStreamInfo, bIsFrameCtsTestFlag, 0x3c);
_Static_assert(sizeof(CedarVideoStreamInfo) == 0x40, "VideoStreamInfo size");

CEDAR_ASSERT_OFF(CedarVideoStreamDataInfo, nLength, 0x08);
CEDAR_ASSERT_OFF(CedarVideoStreamDataInfo, nPts, 0x10);
CEDAR_ASSERT_OFF(CedarVideoStreamDataInfo, bIsFirstPart, 0x20);
CEDAR_ASSERT_OFF(CedarVideoStreamDataInfo, bValid, 0x30);
CEDAR_ASSERT_OFF(CedarVideoStreamDataInfo, pVideoInfo, 0x38);
_Static_assert(sizeof(CedarVideoStreamDataInfo) == 0x40, "VideoStreamDataInfo size");

CEDAR_ASSERT_OFF(CedarVideoPicture, nLineStride, 0x14);
CEDAR_ASSERT_OFF(CedarVideoPicture, nRightOffset, 0x24);
CEDAR_ASSERT_OFF(CedarVideoPicture, nPts, 0x40);
CEDAR_ASSERT_OFF(CedarVideoPicture, pData0, 0x50);
CEDAR_ASSERT_OFF(CedarVideoPicture, nBufId, 0x88);
CEDAR_ASSERT_OFF(CedarVideoPicture, phyYBufAddr, 0x90);
CEDAR_ASSERT_OFF(CedarVideoPicture, phyCBufAddr, 0x98);
CEDAR_ASSERT_OFF(CedarVideoPicture, nBufFd, 0xa8);
CEDAR_ASSERT_OFF(CedarVideoPicture, bFrameErrorFlag, 0xbc);
CEDAR_ASSERT_OFF(CedarVideoPicture, video_full_range_flag, 0xc8);
CEDAR_ASSERT_OFF(CedarVideoPicture, colour_primaries, 0xd4);
CEDAR_ASSERT_OFF(CedarVideoPicture, nBufSize, 0xf8);
CEDAR_ASSERT_OFF(CedarVideoPicture, nCurFrameInfo, 0x108);
_Static_assert(sizeof(CedarVideoPicture) == 0x150, "VideoPicture size");

typedef void CedarDecoder;

typedef CedarDecoder *(*cedar_create_fn)(void);
typedef void (*cedar_destroy_fn)(CedarDecoder *);
typedef int (*cedar_initialize_fn)(CedarDecoder *, CedarVideoStreamInfo *,
                                   CedarVConfig *);
typedef void (*cedar_reset_fn)(CedarDecoder *);
typedef int (*cedar_decode_fn)(CedarDecoder *, int end_of_stream,
                               int keyframe_only, int drop_b_if_late,
                               int64_t now_us);
typedef int (*cedar_request_stream_fn)(CedarDecoder *, int size, char **buf,
                                       int *buf_size, char **ring_buf,
                                       int *ring_size, int stream_index);
typedef int (*cedar_submit_stream_fn)(CedarDecoder *,
                                      CedarVideoStreamDataInfo *,
                                      int stream_index);
typedef CedarVideoPicture *(*cedar_request_picture_fn)(CedarDecoder *,
                                                       int stream_index);
typedef int (*cedar_return_picture_fn)(CedarDecoder *, CedarVideoPicture *);
typedef int (*cedar_count_fn)(CedarDecoder *, int stream_index);
typedef void (*cedar_add_plugin_fn)(char *path);

#endif
