#include <alsa/asoundlib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vmc/audio/alsa_sink.h"
#include "vmc/core/error.h"
#include "vmc/core/logger.h"

typedef struct {
    snd_pcm_t *pcm;
    u64 frames_played;
} alsa_ctx;

#ifdef VMC_DEBUG
static u64 g_xrun_recover;
static u64 g_xrun_fatal;
#endif

/* NVIDIA HDA HDMI outputs are digitally muted (IEC958 playback switch off) by
 * default. Turn the switch on for every IEC958 element so the PCM we write
 * actually reaches the monitor. */
static void alsa_unmute_hdmi(void) {
    snd_mixer_t *m = NULL;
    if (snd_mixer_open(&m, 0) != 0) return;
    if (snd_mixer_attach(m, "hw:1") != 0) {
        snd_mixer_close(m);
        return;
    }
    if (snd_mixer_selem_register(m, NULL, NULL) != 0 ||
        snd_mixer_load(m) != 0) {
        snd_mixer_close(m);
        return;
    }
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e;
         e = snd_mixer_elem_next(e)) {
        const char *name = snd_mixer_selem_get_name(e);
        if (name && strcmp(name, "IEC958") == 0) {
            (void)snd_mixer_selem_set_playback_switch_all(e, 1);
        }
    }
    snd_mixer_close(m);
}

static vmc_status alsa_play(void *ctx, const i16 *pcm, sz_t frames) {
    alsa_ctx *a = (alsa_ctx *)ctx;
    if (!a->pcm) return VMC_OK;
    snd_pcm_sframes_t r = snd_pcm_writei(a->pcm, pcm, (snd_pcm_sframes_t)frames);
    if (r < 0) {
        r = snd_pcm_recover(a->pcm, (int)r, 1);
        if (r < 0) {
#ifdef VMC_DEBUG
            g_xrun_fatal++;
#endif
            VMC_LOGW("alsa: writei failed permanently: %s",
                     snd_strerror((int)r));
            snd_pcm_close(a->pcm);
            a->pcm = NULL;
            return VMC_ERR_IO;
        }
#ifdef VMC_DEBUG
        g_xrun_recover++;
#endif
    }
    if (r > 0)
        a->frames_played += (u64)r;
    return VMC_OK;
}

vmc_status vmc_alsa_sink_init(vmc_audio_sink *sink, const char *device) {
    if (!sink) return VMC_ERR_INVALID_ARG;
    alsa_ctx *a = (alsa_ctx *)calloc(1, sizeof(*a));
    if (!a) return VMC_ERR_NOMEM;
    sink->ctx = a;
    sink->play = alsa_play;

    if (!device) device = getenv("VMC_AUDIO_DEV");
    if (!device) device = "default";

    if (snd_pcm_open(&a->pcm, device, SND_PCM_STREAM_PLAYBACK, 0) != 0) {
        VMC_LOGW("alsa: cannot open '%s' — running silent", device);
        a->pcm = NULL;
        return VMC_OK;
    }
    if (snd_pcm_set_params(a->pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           VMC_AUDIO_CHANNELS, VMC_AUDIO_SAMPLE_RATE, 1,
                           250000) != 0) {
        VMC_LOGW("alsa: set_params failed — running silent");
        snd_pcm_close(a->pcm);
        a->pcm = NULL;
        return VMC_OK;
    }
    VMC_LOGI("alsa: playing 48k stereo via '%s'", device);
    alsa_unmute_hdmi();
    return VMC_OK;
}

void vmc_alsa_sink_close(vmc_audio_sink *sink) {
    if (!sink) return;
    alsa_ctx *a = (alsa_ctx *)sink->ctx;
    if (!a) return;
    if (a->pcm) snd_pcm_close(a->pcm);
    free(a);
    sink->ctx = NULL;
    sink->play = NULL;
}

#ifdef VMC_DEBUG
void vmc_alsa_sink_stats(const vmc_audio_sink *sink, u64 *recover, u64 *fatal) {
    if (recover) *recover = g_xrun_recover;
    if (fatal) *fatal = g_xrun_fatal;
    (void)sink;
}
#endif

bool vmc_alsa_sink_playing(const vmc_audio_sink *sink) {
    if (!sink || !sink->ctx) return false;
    const alsa_ctx *a = (const alsa_ctx *)sink->ctx;
    return a->pcm != NULL;
}

bool vmc_alsa_sink_position_us(const vmc_audio_sink *sink, u64 *pos_us) {
    if (!sink || !sink->ctx || !pos_us) return false;
    const alsa_ctx *a = (const alsa_ctx *)sink->ctx;
    if (!a->pcm) return false;

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return false;
    const u64 wall_us =
        (u64)ts.tv_sec * 1000000ull + (u64)(ts.tv_nsec / 1000);
    /* The ALSA position is the wall time of the sample currently at the DAC.
     * Returning the current wall time makes the audio-master clock the actual
     * playback time, which keeps the video flip locked to the moment audio is
     * heard. The caller uses the delay separately if it needs buffering info. */
    *pos_us = wall_us;
    return true;
}

bool vmc_alsa_sink_delay_us(const vmc_audio_sink *sink, u64 *delay_us) {
    if (!sink || !sink->ctx || !delay_us) return false;
    const alsa_ctx *a = (const alsa_ctx *)sink->ctx;
    if (!a->pcm) return false;

    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(a->pcm, &delay) != 0) return false;
    if (delay < 0) delay = 0;
    *delay_us = (u64)delay * 1000000ull / (u64)VMC_AUDIO_SAMPLE_RATE;
    return true;
}
