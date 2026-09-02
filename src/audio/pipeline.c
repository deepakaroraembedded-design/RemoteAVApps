#include "vmc/audio/pipeline.h"

#include <string.h>

#include "vmc/core/error.h"

void vmc_audio_pipeline_init(vmc_audio_pipeline *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    vmc_mixer_init(&p->mixer);
}

vmc_status vmc_audio_pipeline_render(vmc_audio_pipeline *p, const i16 *mec_pcm,
                                     sz_t mec_frames) {
    if (!p || (mec_frames > 0 && !mec_pcm)) return VMC_ERR_INVALID_ARG;

    vmc_status st = vmc_mixer_set_source(&p->mixer, 0, mec_pcm,
                                         mec_frames, 256);
    if (st != VMC_OK) return st;

    if (!p->sink.play) {
        return VMC_ERR_NOSYS;
    }
    /* Mix only as many frames as the sink can consume at once; the sink
     * plays from the mixed buffer. For now: allocate a small stack block
     * and play it directly. A real implementation uses a frame FIFO. */
    return p->sink.play(p->sink.ctx, mec_pcm, mec_frames);
}

vmc_status vmc_audio_pipeline_capture(vmc_audio_pipeline *p, i16 *buf,
                                      sz_t max_frames, sz_t *out_frames) {
    if (!p || !buf || !out_frames) return VMC_ERR_INVALID_ARG;
    if (!p->source.capture) {
        return VMC_ERR_NOSYS;
    }
    return p->source.capture(p->source.ctx, buf, max_frames, out_frames);
}
