#include "vmc/audio/mixer.h"

#include <string.h>

#include "vmc/core/error.h"

void vmc_mixer_init(vmc_mixer *m) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
}

vmc_status vmc_mixer_set_source(vmc_mixer *m, sz_t idx, const i16 *pcm,
                                sz_t frames, i16 gain_q8) {
    if (!m || idx >= VMC_MIXER_MAX_SOURCES) return VMC_ERR_INVALID_ARG;
    if (frames > 0 && !pcm) return VMC_ERR_INVALID_ARG;
    if (gain_q8 < 0) return VMC_ERR_INVALID_ARG;

    m->src[idx].pcm    = pcm;
    m->src[idx].frames = frames;
    m->src[idx].gain_q8 = gain_q8;
    m->src[idx].active = frames > 0;
    return VMC_OK;
}

void vmc_mixer_clear_source(vmc_mixer *m, sz_t idx) {
    if (!m || idx >= VMC_MIXER_MAX_SOURCES) return;
    m->src[idx].active = false;
}

static inline i16 sat_i16(i32 v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (i16)v;
}

void vmc_mixer_mix(vmc_mixer *m, i16 *out, sz_t frames_out) {
    if (!m || !out || frames_out == 0) return;

    const sz_t samples = frames_out * 2u;

    /* Accumulate in 32-bit to avoid clipping intermediate sums. */
    for (sz_t s = 0; s < samples; s++) {
        i32 acc = 0;
        for (sz_t i = 0; i < VMC_MIXER_MAX_SOURCES; i++) {
            const vmc_mixer_source *src = &m->src[i];
            if (!src->active) continue;
            const sz_t frame = s / 2u;
            if (frame >= src->frames) continue;
            acc += (i32)src->pcm[s] * (i32)src->gain_q8 / 256;
        }
        out[s] = sat_i16(acc);
    }
}
