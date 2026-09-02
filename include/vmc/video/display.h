/*
 * display.h — display/panel output abstraction.
 *
 * Frame consumer: KMS/DRM (fullscreen, zero-copy) on Linux embedded targets,
 * or a dummy/sim backend during development. This is the end of the video
 * pipeline.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_VIDEO_DISPLAY_H
#define VMC_VIDEO_DISPLAY_H

#include "vmc/core/types.h"
#include "vmc/video/decoder.h"

VMC_BEGIN_DECLS

typedef struct vmc_display vmc_display;

typedef struct vmc_display_ops {
    vmc_status (*open)(vmc_display *d, u16 width, u16 height);
    void       (*close)(vmc_display *d);
    /* Present one decoded frame. Implementations may block until vsync. */
    vmc_status (*present)(vmc_display *d, const vmc_video_frame *frame);
} vmc_display_ops;

struct vmc_display {
    const vmc_display_ops *ops;
    void                  *impl;
    u16                    width;
    u16                    height;
    bool                   opened;
};

static inline vmc_status vmc_display_open(vmc_display *d, u16 w, u16 h) {
    return d->ops->open(d, w, h);
}
static inline void vmc_display_close(vmc_display *d) {
    if (d->ops->close) d->ops->close(d);
}
static inline vmc_status vmc_display_present(vmc_display *d,
                                             const vmc_video_frame *f) {
    return d->ops->present(d, f);
}

VMC_END_DECLS

#endif /* VMC_VIDEO_DISPLAY_H */
