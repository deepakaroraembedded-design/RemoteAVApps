/*
 * alsa_sink.h — ALSA playback sink for the thin-client audio pipeline.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_AUDIO_ALSA_SINK_H
#define VMC_AUDIO_ALSA_SINK_H

#include "vmc/audio/pipeline.h"

VMC_BEGIN_DECLS

/* Wire an ALSA sink into the pipeline. `device` may be NULL (uses
 * VMC_AUDIO_DEV env, else "default"). If ALSA/device is unavailable the
 * sink degrades to silent (frames are consumed, not played) and VMC_OK is
 * still returned so the pipeline runs either way. */
vmc_status vmc_alsa_sink_init(vmc_audio_sink *sink, const char *device);

/* Close the ALSA device (safe on a silent sink). */
void vmc_alsa_sink_close(vmc_audio_sink *sink);

#ifdef VMC_DEBUG
/* XRUN counters (recoverable vs fatal), VMC_DEBUG builds only. */
void vmc_alsa_sink_stats(const vmc_audio_sink *sink, u64 *recover, u64 *fatal);
#endif

VMC_END_DECLS

#endif /* VMC_AUDIO_ALSA_SINK_H */
