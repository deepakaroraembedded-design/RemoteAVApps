/*
 * decoder.h — video decode abstraction.
 *
 * Hardware H.264/H.265 decoders (Pi 5 HEVC VPU, RK3588 MPP, Qualcomm) plug in
 * behind this interface. The core pipeline only ever sees decoded frames.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_VIDEO_DECODER_H
#define VMC_VIDEO_DECODER_H

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

typedef enum vmc_video_codec {
    VMC_VIDEO_CODEC_H264 = 0,
    VMC_VIDEO_CODEC_H265 = 1,
    VMC_VIDEO_CODEC_AV1  = 2,
} vmc_video_codec;

/* Decoded frame format. */
typedef enum vmc_pixel_format {
    VMC_PIXFMT_NV12 = 0,
    VMC_PIXFMT_NV21 = 1,
    VMC_PIXFMT_YUV420P = 2,
    VMC_PIXFMT_RGB32 = 3,
} vmc_pixel_format;

typedef struct vmc_video_frame {
    u16 width;
    u16 height;
    vmc_pixel_format pixfmt;
    u32  stride[3];
    u32  ts_us;         /* sender timestamp */
    bool device_mem;    /* planes are CUDA device pointers (no CPU access) */
    const u8 *planes[3];
} vmc_video_frame;

typedef struct vmc_video_decoder vmc_video_decoder;

typedef struct vmc_video_decoder_ops {
    vmc_status (*open)(vmc_video_decoder *d, vmc_video_codec codec,
                       u16 width, u16 height);
    void       (*close)(vmc_video_decoder *d);
    /* Feed one encoded access unit. Returns VMC_OK when a frame is ready
     * in *out, or VMC_ERR_AGAIN when more data is needed. */
    vmc_status (*decode)(vmc_video_decoder *d, const u8 *data, sz_t len,
                         vmc_video_frame *out);
} vmc_video_decoder_ops;

struct vmc_video_decoder {
    const vmc_video_decoder_ops *ops;
    void                        *impl;
    vmc_video_codec              codec;
    bool                         opened;
};

static inline vmc_status vmc_decoder_open(vmc_video_decoder *d,
                                          vmc_video_codec codec,
                                          u16 w, u16 h) {
    return d->ops->open(d, codec, w, h);
}
static inline void vmc_decoder_close(vmc_video_decoder *d) {
    if (d->ops->close) d->ops->close(d);
}
static inline vmc_status vmc_decoder_decode(vmc_video_decoder *d,
                                            const u8 *data, sz_t len,
                                            vmc_video_frame *out) {
    return d->ops->decode(d, data, len, out);
}

VMC_END_DECLS

#endif /* VMC_VIDEO_DECODER_H */
