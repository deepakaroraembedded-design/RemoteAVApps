#include "vmc/video/ffmpeg_decoder.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#ifdef VMC_HAVE_CUDA
#include <libavutil/hwcontext_cuda.h>
#endif
#include <libswscale/swscale.h>

#include "vmc/core/error.h"
#include "vmc/core/logger.h"
#include "vmc/video/fragment.h"

#define DECODER_ALIGNMENT 64u

/* Preferred hardware H.264 decoders, in order. First one that opens wins;
 * software decode is the fallback. */
static const char *const k_hw_decoders[] = {
    "h264_cuvid",   /* NVIDIA (CUVID) */
    "h264_v4l2m2m", /* V4L2 mem2mem (SoC VPUs) */
    "h264_vaapi",   /* Intel/AMD VAAPI */
};

typedef struct avcodec_cx {
    AVCodecContext *ctx;
    AVFrame        *frame;
    AVPacket       *pkt;
    struct SwsContext *sws;
} avcodec_cx;

static vmc_status ffmpeg_open(vmc_video_decoder *d, vmc_video_codec codec,
                              u16 width, u16 height) {
    (void)width;
    (void)height;
    vmc_ffmpeg_decoder *fd = (vmc_ffmpeg_decoder *)d;

    if (codec != VMC_VIDEO_CODEC_H264) {
        VMC_LOGW("ffmpeg: unsupported codec id %d", (int)codec);
        return VMC_ERR_NOSYS;
    }

    /* Try hardware decoders first, then software. */
    const AVCodec *avcodec = NULL;
    const char *used = NULL;
    for (size_t i = 0; i < sizeof(k_hw_decoders) / sizeof(k_hw_decoders[0]); i++) {
        avcodec = avcodec_find_decoder_by_name(k_hw_decoders[i]);
        if (avcodec) {
            used = k_hw_decoders[i];
            break;
        }
    }
    if (!avcodec) {
        avcodec = avcodec_find_decoder(AV_CODEC_ID_H264);
        used = "h264 (software)";
    }
    if (!avcodec) {
        VMC_LOGE("ffmpeg: no H.264 decoder available");
        return VMC_ERR_NOSYS;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(avcodec);
    if (!ctx) return VMC_ERR_NOMEM;

    ctx->thread_count = 1;
    /* CUVID buffers frames unless low-latency mode is enabled. */
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
#ifdef VMC_HAVE_CUDA
    if (fd->output_cuda) {
        /* Provide our own CUDA device context + frames pool so CUVID's
         * cuvidMapVideoFrame / cuMemcpy2DAsync bind to a context we can make
         * current in the decode thread. */
        AVBufferRef *hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
        if (hwdev && av_hwdevice_ctx_init(hwdev) == 0) {
            AVCUDADeviceContext *cuda_dev =
                (AVCUDADeviceContext *)hwdev->data;
            fd->cuda_ctx = (void *)cuda_dev->cuda_ctx;
            ctx->hw_device_ctx = av_buffer_ref(hwdev);
            ctx->hw_frames_ctx = av_hwframe_ctx_alloc(hwdev);
            if (ctx->hw_frames_ctx) {
                AVHWFramesContext *fc =
                    (AVHWFramesContext *)ctx->hw_frames_ctx->data;
                fc->format   = AV_PIX_FMT_CUDA;
                fc->sw_format = AV_PIX_FMT_NV12;
                fc->width    = 3840;
                fc->height   = 2160;
                fc->initial_pool_size = 4;
                if (av_hwframe_ctx_init(ctx->hw_frames_ctx) != 0) {
                    av_buffer_unref(&ctx->hw_frames_ctx);
                    ctx->hw_frames_ctx = NULL;
                }
            }
        }
        av_buffer_unref(&hwdev);
    }
#endif
    if (fd->output_cuda) {
        /* Keep NV12 on the GPU for the Design-B path. */
        av_opt_set(ctx->priv_data, "output_format", "cuda", 0);
    } else {
        /* Copy the decoded frame to system memory as NV12. */
        av_opt_set(ctx->priv_data, "output_format", "nv12", 0);
    }
    /* Force zero-frame decode delay: without it, large (4K) frames stay
     * queued in CUVID and avcodec_receive_frame returns EAGAIN forever. */
    av_opt_set(ctx->priv_data, "delay", "0", 0);
    if (avcodec_open2(ctx, avcodec, NULL) != 0) {
        avcodec_free_context(&ctx);
        if (used && used[0] == 'h' && strncmp(used, "h264 (software)", 16) != 0) {
            /* HW decoder failed to open: retry with software. */
            avcodec = avcodec_find_decoder(AV_CODEC_ID_H264);
            used = "h264 (software)";
            ctx = avcodec_alloc_context3(avcodec);
            if (!ctx) return VMC_ERR_NOMEM;
            if (avcodec_open2(ctx, avcodec, NULL) != 0) {
                avcodec_free_context(&ctx);
                VMC_LOGE("ffmpeg: avcodec_open2 failed (hw + sw)");
                return VMC_ERR_IO;
            }
        } else {
            VMC_LOGE("ffmpeg: avcodec_open2 failed (%s)", used);
            return VMC_ERR_IO;
        }
    }
    VMC_LOGI("ffmpeg: H.264 decoder using %s", used);

    avcodec_cx *cx = (avcodec_cx *)calloc(1, sizeof(*cx));
    if (!cx) {
        avcodec_free_context(&ctx);
        return VMC_ERR_NOMEM;
    }
    cx->ctx   = ctx;
    cx->frame = av_frame_alloc();
    cx->pkt   = av_packet_alloc();
    if (!cx->frame || !cx->pkt) {
        if (cx->frame) av_frame_free(&cx->frame);
        if (cx->pkt) av_packet_free(&cx->pkt);
        free(cx);
        avcodec_free_context(&ctx);
        return VMC_ERR_NOMEM;
    }

    fd->codec_ctx = cx;
    if (fd->out_w == 0 || fd->out_h == 0) {
        fd->out_w = 1920u;
        fd->out_h = 1080u;
    }
    fd->rgb_cap = (u32)fd->out_w * fd->out_h * 4u;
    fd->rgb = (u8 *)malloc(fd->rgb_cap + DECODER_ALIGNMENT);
    fd->in_buf = (u8 *)av_malloc(VMC_VIDEO_AU_MAX + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!fd->rgb || !fd->in_buf) {
        if (cx->frame) av_frame_free(&cx->frame);
        if (cx->pkt) av_packet_free(&cx->pkt);
        free(cx);
        avcodec_free_context(&ctx);
        av_free(fd->in_buf);
        free(fd->rgb);
        fd->codec_ctx = NULL;
        fd->in_buf = NULL;
        fd->rgb = NULL;
        return VMC_ERR_NOMEM;
    }
    fd->opened = true;
    /* Try to load the CUDA NV12->BGRA converter (GPU downscale). */
    fd->cuda_lib = dlopen("libnv12conv.so", RTLD_NOW);
    if (fd->cuda_lib) {
        fd->cuda_conv = (int (*)(const u8 *, const u8 *, int, int, int, int,
                                 u8 *, int, int))dlsym(fd->cuda_lib,
                                                       "nv12_to_bgra");
        if (fd->cuda_conv) {
            VMC_LOGI("ffmpeg: using CUDA NV12->BGRA (libnv12conv.so)");
        } else {
            dlclose(fd->cuda_lib);
            fd->cuda_lib = NULL;
            fd->cuda_conv = NULL;
        }
    }
    VMC_LOGI("ffmpeg: H.264 decoder ready -> %ux%u RGB32",
             (unsigned)fd->out_w, (unsigned)fd->out_h);
    return VMC_OK;
}

static void ffmpeg_close(vmc_video_decoder *d) {
    vmc_ffmpeg_decoder *fd = (vmc_ffmpeg_decoder *)d;
    if (fd->cuda_lib) {
        dlclose(fd->cuda_lib);
        fd->cuda_lib = NULL;
        fd->cuda_conv = NULL;
    }
    avcodec_cx *cx = (avcodec_cx *)fd->codec_ctx;
    if (cx) {
        if (cx->sws) sws_freeContext(cx->sws);
        if (cx->frame) av_frame_free(&cx->frame);
        if (cx->pkt) av_packet_free(&cx->pkt);
        if (cx->ctx) avcodec_free_context(&cx->ctx);
        free(cx);
    }
    av_free(fd->in_buf);
    free(fd->rgb);
    fd->codec_ctx = NULL;
    fd->in_buf = NULL;
    fd->rgb = NULL;
    fd->opened = false;
}

/* Input access units are copied into an aligned buffer with FFmpeg's
 * required trailing padding before being handed to avcodec. */
static vmc_status ffmpeg_decode(vmc_video_decoder *d, const u8 *data, sz_t len,
                                vmc_video_frame *out) {
    vmc_ffmpeg_decoder *fd = (vmc_ffmpeg_decoder *)d;
    avcodec_cx *cx = (avcodec_cx *)fd->codec_ctx;
    if (!fd->opened || !cx || !data || len == 0 || !out) {
        return VMC_ERR_INVALID_ARG;
    }

    if (len > VMC_VIDEO_AU_MAX) return VMC_ERR_OVERRUN;
    memcpy(fd->in_buf, data, len);
    memset(fd->in_buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    av_packet_unref(cx->pkt);
    if (av_new_packet(cx->pkt, (int)len) != 0) {
        return VMC_ERR_NOMEM;
    }
    memcpy(cx->pkt->data, fd->in_buf, len);
    /* CUVID needs monotonic timestamps on packets fed directly (no demuxer),
     * otherwise it holds frames in its reorder queue and never releases. */
    cx->pkt->pts      = fd->next_pts;
    cx->pkt->dts      = fd->next_pts;
    cx->pkt->duration = 1;
    fd->last_send_pts = fd->next_pts;
    fd->next_pts++;

    int rc = avcodec_send_packet(cx->ctx, cx->pkt);
    if (rc < 0) {
        VMC_LOGW("ffmpeg: avcodec_send_packet err %s", av_err2str(rc));
        return VMC_ERR_PROTO;
    }

    rc = avcodec_receive_frame(cx->ctx, cx->frame);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
        return VMC_ERR_AGAIN; /* need more data; no frame yet */
    }
    if (rc < 0) {
        return VMC_ERR_PROTO;
    }
    fd->last_frame_pts = cx->frame->pts;

    if (fd->output_cuda) {
        /* Design B: return device NV12 pointers; conversion happens later on
         * the GPU conversion stream. planes carry CUDA device addresses. */
        memset(out, 0, sizeof(*out));
        out->width      = (u16)cx->frame->width;
        out->height     = (u16)cx->frame->height;
        out->pixfmt     = VMC_PIXFMT_NV12;
        out->stride[0]  = (u32)cx->frame->linesize[0];
        out->stride[1]  = (u32)cx->frame->linesize[1];
        out->device_mem = true;
        out->planes[0]  = (const u8 *)cx->frame->data[0];
        out->planes[1]  = (const u8 *)cx->frame->data[1];
        return VMC_OK;
    }

    if (fd->cuda_conv) {
        /* GPU: NV12 -> BGRA (+ scale) in a CUDA kernel. */
        if (fd->cuda_conv(cx->frame->data[0], cx->frame->data[1],
                          cx->frame->width, cx->frame->height,
                          cx->frame->linesize[0], cx->frame->linesize[1],
                          fd->rgb, fd->out_w, fd->out_h) != 0) {
            return VMC_ERR_IO;
        }
    } else {
        /* CPU: swscale NV12 -> BGRA (+ scale). */
        if (!cx->sws) {
            cx->sws = sws_getContext(cx->frame->width, cx->frame->height,
                                     (enum AVPixelFormat)cx->frame->format,
                                     fd->out_w, fd->out_h, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, NULL, NULL, NULL);
            if (!cx->sws) return VMC_ERR_NOSYS;
        }
        u8 *dst_planes[4] = { fd->rgb, NULL, NULL, NULL };
        const int dst_stride[4] = { (int)fd->out_w * 4, 0, 0, 0 };
        (void)sws_scale(cx->sws, (const u8 *const *)cx->frame->data,
                        cx->frame->linesize, 0, cx->frame->height,
                        (u8 *const *)dst_planes, dst_stride);
    }

    memset(out, 0, sizeof(*out));
    out->width     = fd->out_w;
    out->height    = fd->out_h;
    out->pixfmt    = VMC_PIXFMT_RGB32;
    out->stride[0] = fd->out_w * 4u;
    out->planes[0] = fd->rgb;
    out->ts_us     = 0;
    return VMC_OK;
}

static const vmc_video_decoder_ops k_ffmpeg_ops = {
    .open    = ffmpeg_open,
    .close   = ffmpeg_close,
    .decode  = ffmpeg_decode,
};

vmc_status vmc_ffmpeg_decoder_init(vmc_ffmpeg_decoder *d, u16 out_w, u16 out_h) {
    if (!d) return VMC_ERR_INVALID_ARG;
    memset(d, 0, sizeof(*d));
    d->base.ops = &k_ffmpeg_ops;
    d->base.codec = VMC_VIDEO_CODEC_H264;
    d->out_w = out_w;
    d->out_h = out_h;
    return VMC_OK;
}
