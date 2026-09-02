/*
 * pipeline.h — end-to-end audio pipeline for the thin client.
 *
 * Downlink: transport -> depacketize -> PCM -> jitter FIFO -> mixer -> codec.
 * Uplink:   mic -> PCM -> optional DSP -> packetize -> transport.
 * This header defines the pipeline object and its pluggable stages.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_AUDIO_PIPELINE_H
#define VMC_AUDIO_PIPELINE_H

#include <stddef.h>

#include "vmc/core/types.h"
#include "vmc/audio/mixer.h"

VMC_BEGIN_DECLS

#define VMC_AUDIO_SAMPLE_RATE 48000
#define VMC_AUDIO_CHANNELS    2
#define VMC_AUDIO_FRAME_MS    10   /* packet cadence */

/* Downlink stage: called with one PCM frame block to play (already mixed). */
typedef struct vmc_audio_sink {
    vmc_status (*play)(void *ctx, const i16 *pcm, sz_t frames);
    void *ctx;
} vmc_audio_sink;

/* Uplink stage: called with captured mic PCM to send toward MEC. */
typedef struct vmc_audio_source {
    vmc_status (*capture)(void *ctx, i16 *pcm, sz_t max_frames,
                          sz_t *out_frames);
    void *ctx;
} vmc_audio_source;

typedef struct vmc_audio_pipeline {
    vmc_mixer       mixer;
    vmc_audio_sink  sink;
    vmc_audio_source source;
    u32             last_mix_ms;
} vmc_audio_pipeline;

void vmc_audio_pipeline_init(vmc_audio_pipeline *p);

/* Mix downlink (source 0 = MEC PCM) and hand to sink. */
vmc_status vmc_audio_pipeline_render(vmc_audio_pipeline *p, const i16 *mec_pcm,
                                     sz_t mec_frames);

/* Pull a mic block and forward upstream. */
vmc_status vmc_audio_pipeline_capture(vmc_audio_pipeline *p, i16 *buf,
                                      sz_t max_frames, sz_t *out_frames);

VMC_END_DECLS

#endif /* VMC_AUDIO_PIPELINE_H */
