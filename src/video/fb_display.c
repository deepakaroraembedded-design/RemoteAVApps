#include "vmc/video/fb_display.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vmc/core/error.h"
#include "vmc/core/logger.h"

static vmc_status fb_open(vmc_display *d, u16 width, u16 height) {
    vmc_fb_display *fb = (vmc_fb_display *)d;
    (void)width;
    (void)height;
    /* Geometry comes from the actual device, not the caller. */
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) != 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        VMC_LOGE("fb: FBIOGET_*SCREENINFO failed");
        return VMC_ERR_IO;
    }
    fb->xres     = (u16)vinfo.xres;
    fb->yres     = (u16)vinfo.yres;
    fb->bpp      = (u16)vinfo.bits_per_pixel;
    fb->line_len = (u32)finfo.line_length;
    fb->map_size = (sz_t)fb->line_len * (vinfo.yres_virtual > 0
                                             ? vinfo.yres_virtual
                                             : vinfo.yres);

    fb->map = (u8 *)mmap(NULL, fb->map_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fb->fd, 0);
    if (fb->map == MAP_FAILED) {
        VMC_LOGE("fb: mmap failed");
        fb->map = NULL;
        return VMC_ERR_IO;
    }

    d->width  = fb->xres;
    d->height = fb->yres;
    d->opened = true;
    VMC_LOGI("fb: %ux%u @ %u bpp (stride %u) mapped %zu bytes",
             (unsigned)fb->xres, (unsigned)fb->yres, (unsigned)fb->bpp,
             fb->line_len, fb->map_size);
    return VMC_OK;
}

static void fb_close(vmc_display *d) {
    vmc_fb_display *fb = (vmc_fb_display *)d;
    if (fb->map) {
        (void)munmap(fb->map, fb->map_size);
        fb->map = NULL;
    }
    if (fb->fd >= 0) {
        (void)close(fb->fd);
        fb->fd = -1;
    }
    d->opened = false;
}

static vmc_status fb_present(vmc_display *d, const vmc_video_frame *frame) {
    vmc_fb_display *fb = (vmc_fb_display *)d;
    if (!fb->map || !frame) return VMC_ERR_INVALID_ARG;
    if (frame->pixfmt != VMC_PIXFMT_RGB32) return VMC_ERR_NOSYS;

    const u32 src_stride = frame->stride[0];
    const u8 *src = frame->planes[0];
    const u32 w = frame->width < fb->xres ? frame->width : fb->xres;
    const u32 h = frame->height < fb->yres ? frame->height : fb->yres;

    /* Copy scanline by scanline so mismatched strides stay safe. */
    for (u32 y = 0; y < h; y++) {
        memcpy(fb->map + (sz_t)y * fb->line_len,
               src + (sz_t)y * src_stride,
               (sz_t)w * 4u);
    }
    return VMC_OK;
}

static const vmc_display_ops k_fb_ops = {
    .open    = fb_open,
    .close   = fb_close,
    .present = fb_present,
};

vmc_status vmc_fb_display_init(vmc_fb_display *fb, const char *device_path) {
    if (!fb) return VMC_ERR_INVALID_ARG;
    memset(fb, 0, sizeof(*fb));
    fb->base.ops = &k_fb_ops;
    fb->fd = -1;

    if (!device_path) device_path = "/dev/fb0";
    fb->fd = open(device_path, O_RDWR);
    if (fb->fd < 0) {
        VMC_LOGW("fb: cannot open %s", device_path);
        return VMC_ERR_IO;
    }
    return VMC_OK;
}
