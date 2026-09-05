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

/* True when the PCM device is open and playing (false = silent sink). */
bool vmc_alsa_sink_playing(const vmc_audio_sink *sink);

/* Return the wall-clock time (in microseconds) of the sample currently being
 * heard by the ALSA sink. This is the current wall time, because the caller
 * anchors the video timeline to when audio is heard. Returns false if the
 * sink is silent or the position cannot be queried. */
bool vmc_alsa_sink_position_us(const vmc_audio_sink *sink, u64 *pos_us);

/* Return the current ALSA buffering delay in microseconds (the time between
 * writing a sample and it reaching the DAC). Returns false if the sink is
 * silent or the delay cannot be queried. */
bool vmc_alsa_sink_delay_us(const vmc_audio_sink *sink, u64 *delay_us);

#ifdef VMC_DEBUG
/* XRUN counters (recoverable vs fatal), VMC_DEBUG builds only. */
void vmc_alsa_sink_stats(const vmc_audio_sink *sink, u64 *recover, u64 *fatal);
#endif

VMC_END_DECLS

#endif /* VMC_AUDIO_ALSA_SINK_H */
