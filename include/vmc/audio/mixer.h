/*
 * mixer.h — fixed-point audio mixing engine.
 *
 * Blends multiple normalized PCM sources (MEC downlink, local mic sidetone,
 * USB/BLE sources, DSP/system tones) into one output buffer. Saturated
 * 16-bit stereo PCM. RTOS-friendly: no allocation, caller buffers.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_AUDIO_MIXER_H
#define VMC_AUDIO_MIXER_H

#include <stddef.h>

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

#define VMC_MIXER_MAX_SOURCES 8

typedef struct vmc_mixer_source {
    bool   active;
    i16    gain_q8;    /* Q8.8 fixed point; 1.0 = 256 */
    const i16 *pcm;    /* interleaved stereo, caller-owned */
    sz_t   frames;
} vmc_mixer_source;

typedef struct vmc_mixer {
    vmc_mixer_source src[VMC_MIXER_MAX_SOURCES];
} vmc_mixer;

void vmc_mixer_init(vmc_mixer *m);

/* Configure source idx. Returns VMC_ERR_INVALID_ARG on bad idx/gain. */
vmc_status vmc_mixer_set_source(vmc_mixer *m, sz_t idx, const i16 *pcm,
                                sz_t frames, i16 gain_q8);

void vmc_mixer_clear_source(vmc_mixer *m, sz_t idx);

/* Mix all active sources into out (interleaved stereo frames_out samples
 * per channel; total samples = frames_out * 2). Zeroes unused tail. */
void vmc_mixer_mix(vmc_mixer *m, i16 *out, sz_t frames_out);

VMC_END_DECLS

#endif /* VMC_AUDIO_MIXER_H */
