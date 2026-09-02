/*
 * fb_display.h — Linux framebuffer (/dev/fb0) display backend.
 *
 * Presents RGB32 frames by mmapping the kernel's framebuffer directly —
 * no X, no Wayland, no DRM master needed. Works with the simpledrm/drmfb
 * devices used by headless thin clients. This is the display end of the
 * video pipeline.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_VIDEO_FB_DISPLAY_H
#define VMC_VIDEO_FB_DISPLAY_H

#include "vmc/core/types.h"
#include "vmc/video/display.h"

VMC_BEGIN_DECLS

typedef struct vmc_fb_display {
    vmc_display base;      /* must be first */
    int         fd;
    u8         *map;       /* mmap'ed framebuffer */
    sz_t        map_size;
    u32         line_len;  /* bytes per scanline (may exceed width*4) */
    u16         xres;      /* visible width */
    u16         yres;      /* visible height */
    u16         bpp;       /* bits per pixel */
} vmc_fb_display;

/* Prepare the backend for device_path (e.g. "/dev/fb0"). */
vmc_status vmc_fb_display_init(vmc_fb_display *d, const char *device_path);

VMC_END_DECLS

#endif /* VMC_VIDEO_FB_DISPLAY_H */
