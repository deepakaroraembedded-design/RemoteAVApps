#include "vmc/video/drm_scanout.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libdrm/drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "vmc/core/error.h"
#include "vmc/core/logger.h"
#include "vmc/core/platform.h"

#define XRGB8888 0x34325258u

static void flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                         unsigned int tv_usec, void *user) {
    (void)fd;
    (void)seq;
    /* tv_sec/tv_usec are CLOCK_MONOTONIC at flip-complete (vsync). */
    vmc_drm_scanout *s = (vmc_drm_scanout *)user;
    const u64 ts = (u64)tv_sec * 1000000u + (u64)tv_usec;
    /* One event maps to one FIFO record (the buffer that was actually
     * submitted). Relying on the single flip_pending field desyncs when two
     * events ever arrive in one drain: the second event would be dropped, the
     * buffer left busy forever, and the pipeline would freeze or drop to
     * 30 fps. Consume the oldest outstanding flip in order. */
    if (s->flip_h_head != s->flip_h_tail) {
        const vmc_drm_flip_rec rec = s->flip_history[s->flip_h_tail];
        s->flip_h_tail = (s->flip_h_tail + 1) % VMC_DRM_FLIP_HISTORY;
        if (s->on_screen >= 0) {
            vmc_drm_buffer *old_b = &s->bufs[s->on_screen];
            old_b->busy = false;
            old_b->last_flip_ts = ts;
        }
        s->on_screen = rec.buf_idx;
        s->last_completed = rec.buf_idx;
        s->flip_pending = -1;
    } else if (s->flip_pending >= 0) {
        /* Fallback for a stray event without a FIFO record. */
        if (s->on_screen >= 0) {
            vmc_drm_buffer *old_b = &s->bufs[s->on_screen];
            old_b->busy = false;
            old_b->last_flip_ts = ts;
        }
        s->on_screen = s->flip_pending;
        s->last_completed = s->flip_pending;
        s->flip_pending = -1;
    }
    s->flips_done++;
    s->last_flip_ts_us = ts;
}

static u32 create_dumb(int fd, u32 w, u32 h, u32 *pitch, u64 *size) {
    struct drm_mode_create_dumb cd = {0};
    cd.width = w; cd.height = h; cd.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) return 0;
    *pitch = cd.pitch;
    *size = cd.size;
    return cd.handle;
}

static void *map_dumb(int fd, u32 handle, u64 size) {
    struct drm_mode_map_dumb md = {0};
    md.handle = handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) return NULL;
    return mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, (off_t)md.offset);
}

static int find_connected(drmModeRes *res, int fd, u32 *conn,
                          drmModeModeInfo *mode) {
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            *conn = c->connector_id;
            /* Prefer the connector's preferred mode; fall back to the
             * highest resolution so we never end up on an oddball default. */
            int best = 0;
            for (int m = 1; m < c->count_modes; m++) {
                const drmModeModeInfo *b = &c->modes[best];
                const drmModeModeInfo *n = &c->modes[m];
                const bool n_pref = (n->type & DRM_MODE_TYPE_PREFERRED) != 0;
                const bool b_pref = (b->type & DRM_MODE_TYPE_PREFERRED) != 0;
                if (n_pref && !b_pref) {
                    best = m;
                } else if (n_pref == b_pref &&
                           (u64)n->hdisplay * n->vdisplay >
                               (u64)b->hdisplay * b->vdisplay) {
                    best = m;
                }
            }
            *mode = c->modes[best];
            drmModeFreeConnector(c);
            return 0;
        }
        if (c) drmModeFreeConnector(c);
    }
    return -1;
}

vmc_status vmc_drm_scanout_init(vmc_drm_scanout *s, const char *dev, int nbufs) {
    if (!s || nbufs < 1 || nbufs > VMC_DRM_MAX_BUFS) return VMC_ERR_INVALID_ARG;
    memset(s, 0, sizeof(*s));
    s->nbufs = nbufs;
    if (!dev) dev = "/dev/dri/card1";

    s->fd = open(dev, O_RDWR);
    if (s->fd < 0) { VMC_LOGW("drm: cannot open %s", dev); return VMC_ERR_IO; }

    drmModeRes *res = drmModeGetResources(s->fd);
    if (!res) { close(s->fd); s->fd = -1; return VMC_ERR_IO; }
    drmModeModeInfo mode;
    if (find_connected(res, s->fd, &s->conn, &mode) < 0) {
        VMC_LOGW("drm: no connected connector");
        drmModeFreeResources(res);
        close(s->fd); s->fd = -1;
        return VMC_ERR_NOT_FOUND;
    }
    s->crtc = res->crtcs[0];
    s->w = mode.hdisplay;
    s->h = mode.vdisplay;
    s->flip_pending = -1;
    s->on_screen = 0;
    s->vrefresh = mode.vrefresh;
    s->vblank_period_us = (mode.vrefresh > 0) ? 1000000u / mode.vrefresh : 16667u;
    s->mode = malloc(sizeof(mode));
    if (s->mode) memcpy(s->mode, &mode, sizeof(mode));
    VMC_LOGI("drm: conn=%u crtc=%u %ux%u@%u vblank=%uus", s->conn, s->crtc, s->w, s->h,
             mode.vrefresh, s->vblank_period_us);

    for (int i = 0; i < nbufs; i++) {
        u64 size;
        u32 handle = create_dumb(s->fd, s->w, s->h, &s->bufs[i].pitch, &size);
        if (!handle) { close(s->fd); s->fd = -1; return VMC_ERR_IO; }
        s->bufs[i].handle = handle;
        s->bufs[i].size = (sz_t)size;
        s->bufs[i].map = map_dumb(s->fd, handle, size);
        if (!s->bufs[i].map) { close(s->fd); s->fd = -1; return VMC_ERR_IO; }
        memset(s->bufs[i].map, 0, (size_t)size);

        u32 handles[4] = {handle, 0, 0, 0};
        u32 pitches[4] = {s->bufs[i].pitch, 0, 0, 0};
        u32 offsets[4] = {0, 0, 0, 0};
        if (drmModeAddFB2(s->fd, s->w, s->h, XRGB8888, handles, pitches,
                          offsets, &s->bufs[i].fb, 0) < 0) {
            VMC_LOGW("drm: AddFB2 failed for buf %d", i);
            close(s->fd); s->fd = -1;
            return VMC_ERR_IO;
        }
        s->bufs[i].prime_fd = -1;
        if (drmPrimeHandleToFD(s->fd, handle, DRM_CLOEXEC | DRM_RDWR,
                               &s->bufs[i].prime_fd) < 0) {
            VMC_LOGW("drm: prime export failed for buf %d: %s", i,
                     strerror(errno));
            s->bufs[i].prime_fd = -1;
        }
        VMC_LOGI("drm: buf %d handle=%u fb=%u pitch=%u size=%zu prime_fd=%d", i, handle,
                 s->bufs[i].fb, s->bufs[i].pitch, s->bufs[i].size,
                 s->bufs[i].prime_fd);
    }

    /* Initial CRTC set with buffer 0 (black). */
    if (drmModeSetCrtc(s->fd, s->crtc, s->bufs[0].fb, 0, 0, &s->conn, 1,
                       &mode) < 0) {
        VMC_LOGW("drm: SetCrtc failed: %s", strerror(errno));
        close(s->fd); s->fd = -1;
        return VMC_ERR_IO;
    }
    s->on_screen = 0;         /* now scanning out buffer 0 */
    s->crtc_set = true;
    s->next = 1;
    VMC_LOGI("drm: scanout ready (%d buffers)", nbufs);
    return VMC_OK;
}

void *vmc_drm_scanout_next(vmc_drm_scanout *s) {
    int idx = -1;
    void *m = vmc_drm_scanout_next_idx(s, &idx);
    return m;
}

void *vmc_drm_scanout_next_idx(vmc_drm_scanout *s, int *out_idx) {
    for (int i = 0; i < s->nbufs; i++) {
        int idx = (s->next + i) % s->nbufs;
        if (!s->bufs[idx].busy && idx != s->on_screen &&
            idx != s->flip_pending) {
            s->next = (idx + 1) % s->nbufs;
            s->last_presented = (u32)idx;
            s->bufs[idx].busy = true; /* held until flip-complete event */
            if (out_idx) *out_idx = idx;
            return s->bufs[idx].map;
        }
    }
    {
        static int nbusy_warn = 0;
        if ((nbusy_warn++ % 10) == 0) {
            VMC_LOGW("drm next_idx: all buffers busy (on_screen=%d pending=%d busy=%d,%d,%d,%d,%d)",
                     s->on_screen, s->flip_pending,
                     s->bufs[0].busy, s->bufs[1].busy, s->bufs[2].busy,
                     s->bufs[3].busy, s->bufs[4].busy);
        }
    }
    return NULL;   /* all buffers busy (in flight / on screen) */
}

vmc_status vmc_drm_scanout_present(vmc_drm_scanout *s, int idx) {
    if (idx < 0) idx = (int)s->last_presented;
    if (idx < 0 || idx >= s->nbufs) {
        VMC_LOGW("drm: invalid scanout buffer index %d", idx);
        return VMC_ERR_INVALID_ARG;
    }
    /* Serialize flips: wait for the previous flip to complete before
     * submitting the next (the driver is event-reliable under this pattern —
     * see the flip probe). A lost completion event must never block the
     * pipeline, so bound the wait and force-resync the CRTC when stuck. */
    for (int w = 0; w < 90 && s->flip_pending >= 0; w++) {
        (void)vmc_drm_scanout_wait_flip(s, (int)(s->vblank_period_us / 1000u));
    }
    if (s->flip_pending >= 0) {
        const int stuck = s->flip_pending;
        VMC_LOGW("drm: flip %d lost its event; force-resync CRTC", stuck);
        if (s->on_screen >= 0 && s->mode) {
            (void)drmModeSetCrtc(s->fd, s->crtc, s->bufs[s->on_screen].fb,
                                 0, 0, &s->conn, 1,
                                 (const drmModeModeInfo *)s->mode);
        }
        s->bufs[stuck].busy = false;
        s->flip_pending = -1;
        s->flip_h_head = s->flip_h_tail = 0;
    }
    for (int tries = 0; tries < 3; tries++) {
        if (drmModePageFlip(s->fd, s->crtc, s->bufs[idx].fb,
                            DRM_MODE_PAGE_FLIP_EVENT, s) == 0) {
            s->bufs[idx].busy = true;
            s->flip_pending = idx;
            s->bufs[idx].submit_wall_us = vmc_time_now_us();
            if ((s->flip_h_head + 1) % VMC_DRM_FLIP_HISTORY == s->flip_h_tail)
                s->flip_h_tail = (s->flip_h_tail + 1) % VMC_DRM_FLIP_HISTORY;
            s->flip_history[s->flip_h_head] = (vmc_drm_flip_rec){
                .buf_idx = idx, .submit_us = s->bufs[idx].submit_wall_us};
            s->flip_h_head = (s->flip_h_head + 1) % VMC_DRM_FLIP_HISTORY;
            return VMC_OK;
        }
        if (errno == EBUSY) {
            /* Should not happen after the serialization wait; one more drain
             * then give up cleanly rather than spin. */
            (void)vmc_drm_scanout_wait_flip(s, (int)(s->vblank_period_us / 1000u));
            continue;
        }
        VMC_LOGW("drm: PageFlip failed: %s", strerror(errno));
        return VMC_ERR_IO;
    }
    return VMC_ERR_AGAIN;
}

static int drain_events(vmc_drm_scanout *s, int timeout_ms) {
    /* Single-pass drain with a SHARED deadline: wait up to timeout_ms for the
     * first event, then keep consuming bursts until the deadline, but never
     * restart a full timeout after handling an event (the old code blocked
     * another 20ms in a second poll, which turned the EBUSY path into a
     * ~33ms-per-frame stall and halved the flip cadence to 30fps). */
    int done = 0;
    u64 deadline_ms = vmc_time_now_ms() + (u64)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        int rem = (int)(deadline_ms - vmc_time_now_ms());
        if (rem < 0) rem = 0;
        if (rem > timeout_ms) rem = timeout_ms;
        struct pollfd pfd = { s->fd, POLLIN, 0 };
        int r = poll(&pfd, 1, rem);
        if (r <= 0) break;
        drmEventContext ev;
        memset(&ev, 0, sizeof(ev));
        ev.version = DRM_EVENT_CONTEXT_VERSION;
        ev.page_flip_handler = flip_handler;
        (void)drmHandleEvent(s->fd, &ev);
        done++;
        if (timeout_ms == 0) break;
    }
    (void)s;
    return done;
}

int vmc_drm_scanout_wait_flip(vmc_drm_scanout *s, int timeout_ms) {
    return drain_events(s, timeout_ms);
}

int vmc_drm_scanout_drain(vmc_drm_scanout *s) {
    return drain_events(s, 0);
}

void vmc_drm_scanout_close(vmc_drm_scanout *s) {
    if (s->fd >= 0) {
        for (int i = 0; i < s->nbufs; i++) {
            if (s->bufs[i].map) munmap(s->bufs[i].map, s->bufs[i].size);
            if (s->bufs[i].fb) drmModeRmFB(s->fd, s->bufs[i].fb);
            if (s->bufs[i].prime_fd >= 0) close(s->bufs[i].prime_fd);
            if (s->bufs[i].handle) {
                struct drm_mode_destroy_dumb dd = {0};
                dd.handle = s->bufs[i].handle;
                (void)drmIoctl(s->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            }
        }
        close(s->fd);
        s->fd = -1;
    }
    free(s->mode);
    s->mode = NULL;
}