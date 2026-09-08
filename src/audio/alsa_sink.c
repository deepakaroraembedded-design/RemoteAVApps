#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "vmc/audio/alsa_sink.h"
#include "vmc/core/error.h"
#include "vmc/core/logger.h"
#include "vmc/core/telemetry.h"

typedef struct {
    snd_pcm_t *pcm;
    u64 frames_played;
} alsa_ctx;

#ifdef VMC_DEBUG
static u64 g_xrun_recover;
static u64 g_xrun_fatal;
#endif

/* Detect an attached USB audio device (USB headset) by scanning
 * /proc/asound/cards for a USB-Audio card that exposes a playback PCM. Writes
 * the ALSA device string "plughw:<card-index>,0" into out. Returns true if
 * found. plughw is used (rather than hw) so the PCM handles any rate/format
 * the USB device does not natively accept (the pipeline always writes 48k
 * s16). The card INDEX is used (not the padded name in the [ ] column) so the
 * device string is always a valid ALSA PCM name. */
bool vmc_alsa_find_usb_device(char *out, size_t out_len) {
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int card = -1;
        char driver[64] = {0};
        /* " 0 [Seri           ]: USB-Audio - Plantronics ..." */
        if (sscanf(line, " %d [%*[^]]]: %63s -", &card, driver) == 2 &&
            strcmp(driver, "USB-Audio") == 0) {
            char pcm_path[128];
            snprintf(pcm_path, sizeof(pcm_path),
                     "/proc/asound/card%d/pcm0p/sub0/info", card);
            struct stat st;
            if (stat(pcm_path, &st) == 0) {
                snprintf(out, out_len, "plughw:%d,0", card);
                fclose(f);
                return true;
            }
        }
    }
    fclose(f);
    return false;
}

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
        if (vmc_tlm_enabled())
            vmc_tlm_emit("buf", "\"which\":\"audio_fifo\","
                         "\"event\":\"underflow\",\"level\":0,"
                         "\"cap\":2097152,\"count\":1");
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

    /* Prefer a physically-attached USB headset over the configured sink (the
     * HDMI display): audio follows the device actually in front of the user.
     * Falls back to VMC_AUDIO_DEV / "default" when no USB audio is present. */
    char usb_dev[96] = {0};
    const bool use_usb = vmc_alsa_find_usb_device(usb_dev, sizeof(usb_dev));
    const char *dev = use_usb ? usb_dev : device;
    if (use_usb) {
        VMC_LOGI("alsa: USB audio device detected — routing to '%s'", usb_dev);
    } else {
        if (!device) device = getenv("VMC_AUDIO_DEV");
        if (!device) device = "default";
        dev = device;
    }

    if (snd_pcm_open(&a->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0) != 0) {
        VMC_LOGW("alsa: cannot open '%s' — running silent", dev);
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
    VMC_LOGI("alsa: playing 48k stereo via '%s'", dev);
    /* The NVIDIA HDMI is digitally muted by default; only unmute it when the
     * audio is actually routed there (a USB headset needs no HDMI unmute). */
    if (!use_usb) alsa_unmute_hdmi();
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

bool vmc_alsa_sink_frames_played(const vmc_audio_sink *sink, u64 *frames) {
    if (!sink || !sink->ctx || !frames) return false;
    const alsa_ctx *a = (const alsa_ctx *)sink->ctx;
    if (!a->pcm) return false;
    *frames = a->frames_played;
    return true;
}
