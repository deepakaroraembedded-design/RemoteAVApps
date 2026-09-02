/*
 * ffmpeg_decoder.h — libavcodec H.264 software decoder backend.
 *
 * Implements vmc_video_decoder_ops. Decodes H.264 access units and converts
 * to RGB32 (BGRA memory order) via libswscale so the result can be handed
 * straight to a framebuffer/DRM display. Only compiled when FFmpeg dev
 * headers are present (see CMake).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_VIDEO_FFMPEG_DECODER_H
#define VMC_VIDEO_FFMPEG_DECODER_H

#include "vmc/core/types.h"
#include "vmc/video/decoder.h"

VMC_BEGIN_DECLS

typedef struct vmc_ffmpeg_decoder {
    vmc_video_decoder base;   /* must be first */
    void             *codec_ctx;   /* AVCodecContext* */
    void             *frame;       /* AVFrame* (decoded) */
    void             *packet;      /* AVPacket* */
    void             *sws;         /* SwsContext* */
    u8               *in_buf;      /* av_malloc'd AU staging + padding */
    u8               *rgb;         /* converted RGB32 output */
    i64               next_pts;    /* monotonic packet timestamp counter */
    i64               last_send_pts;   /* pts of the packet just decoded */
    i64               last_frame_pts;  /* pts of the frame last received */
    void             *cuda_lib;        /* dlopen'd libnv12conv.so */
    int             (*cuda_conv)(const u8 *, const u8 *, int, int, int, int,
                                 u8 *, int, int);
    void             *cuda_ctx;        /* FFmpeg CUDA hw_device_ctx context */
    bool              output_cuda;     /* decode to CUDA device NV12 */
    u32               rgb_cap;
    u16               out_w;       /* target width  (display) */
    u16               out_h;       /* target height (display) */
    bool              opened;
} vmc_ffmpeg_decoder;

/* out_w/out_h are the presentation size the decoder scales to. */
vmc_status vmc_ffmpeg_decoder_init(vmc_ffmpeg_decoder *d, u16 out_w, u16 out_h);

VMC_END_DECLS

#endif /* VMC_VIDEO_FFMPEG_DECODER_H */
