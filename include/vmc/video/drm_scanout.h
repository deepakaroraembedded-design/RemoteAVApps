/*
 * drm_scanout.h — GPU scanout via DRM dumb buffers + page-flip events.
 *
 * Part of the Design-B path: decoded frames are converted on the GPU into
 * these dumb XRGB buffers (one D2H), then presented with drmModePageFlip.
 * Flip-complete events (vsync-aligned) free the buffer for reuse, so the
 * host CPU knows exactly when a frame is on screen.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_VIDEO_DRM_SCANOUT_H
#define VMC_VIDEO_DRM_SCANOUT_H

#include <stddef.h>

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

#define VMC_DRM_MAX_BUFS 3

typedef struct vmc_drm_buffer {
    u32     handle;
    u32     fb;
    u32     pitch;
    sz_t    size;
    void   *map;      /* host (BGRA) mapping, the D2H target */
    bool    busy;     /* flipped and not yet flip-completed */
    u64     last_flip_ts; /* monotonic us when this buffer was scanned out */
} vmc_drm_buffer;

typedef struct vmc_drm_scanout {
    int            fd;
    u32            crtc;
    u32            conn;
    u32            w;
    u32            h;
    int            nbufs;
    int            next;      /* round-robin write cursor */
    u32            last_presented; /* buffer idx reserved by next_idx */
    int            flip_pending;   /* buffer idx submitted to drmModePageFlip
                                      (or -1 if none pending) */
    u64            last_flip_ts_us; /* monotonic us when last flip completed */
    u32            flips_done;   /* total flip-complete events */
    vmc_drm_buffer bufs[VMC_DRM_MAX_BUFS];
    bool           crtc_set;
} vmc_drm_scanout;

/* Open card (auto-select a connected connector), create nbufs dumb XRGB
 * buffers at the connector's native mode, and set the CRTC. */
vmc_status vmc_drm_scanout_init(vmc_drm_scanout *s, const char *dev, int nbufs);

/* Return the host mapping of the next free buffer (or NULL if all busy),
 * and mark it as the pending write target. */
void *vmc_drm_scanout_next(vmc_drm_scanout *s);

/* Like vmc_drm_scanout_next but also returns the buffer index. */
void *vmc_drm_scanout_next_idx(vmc_drm_scanout *s, int *out_idx);

/* Present the buffer last returned by vmc_drm_scanout_next via page-flip.
 * Returns VMC_ERR_AGAIN if a flip is still pending on the CRTC. */
vmc_status vmc_drm_scanout_present(vmc_drm_scanout *s);

/* Block (up to timeout_ms) for flip-complete events; marks buffers free.
 * Returns the number of flips completed, or <0 on error. */
int vmc_drm_scanout_wait_flip(vmc_drm_scanout *s, int timeout_ms);

/* Non-blocking drain of any pending flip events. */
int vmc_drm_scanout_drain(vmc_drm_scanout *s);

void vmc_drm_scanout_close(vmc_drm_scanout *s);

VMC_END_DECLS

#endif /* VMC_VIDEO_DRM_SCANOUT_H */
