#include "vmc/input/evdev_input.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vmc/core/error.h"
#include "vmc/core/logger.h"
#include "vmc/core/platform.h"

static void evdev_close(vmc_input_capture *c) {
    vmc_evdev_input *in = (vmc_evdev_input *)c;
    if (in->fd >= 0) {
        (void)close(in->fd);
        in->fd = -1;
    }
}

static vmc_status evdev_open(vmc_input_capture *c) {
    vmc_evdev_input *in = (vmc_evdev_input *)c;
    (void)in;
    return VMC_OK; /* fd opened at init for simplicity */
}

static vmc_status evdev_poll(vmc_input_capture *c, vmc_input_event *ev) {
    vmc_evdev_input *in = (vmc_evdev_input *)c;
    if (in->fd < 0) return VMC_ERR_IO;

    static struct input_event ie;
    ssize_t n;
    u32 tx = 0, ty = 0, pressure = 0, slot = 0;
    bool have_tx = false, have_ty = false;

    /* Drain until a SYN_REPORT or until no more data is available. */
    for (;;) {
        n = read(in->fd, &ie, sizeof(ie));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return VMC_ERR_IO;
        }
        if ((size_t)n != sizeof(ie)) {
            continue;
        }
        switch (ie.type) {
            case EV_ABS:
                switch (ie.code) {
                    case ABS_MT_SLOT:        slot = (u32)ie.value; break;
                    case ABS_MT_POSITION_X:  tx = (u32)ie.value; have_tx = true; break;
                    case ABS_MT_POSITION_Y:  ty = (u32)ie.value; have_ty = true; break;
                    case ABS_MT_PRESSURE:    pressure = (u32)ie.value; break;
                    case ABS_MT_TRACKING_ID:
                        if (ie.value < 0) {
                            /* touch lift */
                            memset(ev, 0, sizeof(*ev));
                            ev->type  = VMC_INPUT_TOUCH_UP;
                            ev->slot  = (u16)slot;
                            ev->ts_us = (u32)vmc_time_now_us();
                            return VMC_OK;
                        }
                        break;
                    default: break;
                }
                break;
            case EV_KEY:
                if (ie.code == BTN_TOUCH || ie.code == BTN_TOOL_FINGER) {
                    if (ie.value == 0) {
                        memset(ev, 0, sizeof(*ev));
                        ev->type  = VMC_INPUT_TOUCH_UP;
                        ev->slot  = (u16)slot;
                        ev->ts_us = (u32)vmc_time_now_us();
                        return VMC_OK;
                    }
                }
                break;
            case EV_SYN:
                if (ie.code == SYN_REPORT) {
                    if (have_tx || have_ty) {
                        memset(ev, 0, sizeof(*ev));
                        ev->type     = VMC_INPUT_TOUCH_MOVE;
                        ev->slot     = (u16)slot;
                        ev->x        = (u16)tx;  /* raw; MEC knows panel */
                        ev->y        = (u16)ty;
                        ev->pressure = (u16)pressure;
                        ev->ts_us    = (u32)vmc_time_now_us();
                        return VMC_OK;
                    }
                }
                break;
            default:
                break;
        }
    }

    /* Give the caller a chance to schedule between batches. */
    return VMC_ERR_AGAIN;
}

static const vmc_input_ops k_evdev_ops = {
    .open  = evdev_open,
    .close = evdev_close,
    .poll  = evdev_poll,
};

vmc_status vmc_evdev_init(vmc_evdev_input *in, const char *device_path) {
    if (!in) return VMC_ERR_INVALID_ARG;
    memset(in, 0, sizeof(*in));
    in->base.ops = &k_evdev_ops;
    in->fd = -1;

    if (!device_path) {
        device_path = "/dev/input/event0";
    }
    in->fd = open(device_path, O_RDONLY | O_NONBLOCK);
    if (in->fd < 0) {
        VMC_LOGW("evdev: cannot open %s: %s", device_path, strerror(errno));
        return VMC_ERR_IO;
    }

    /* Query axis extents for normalization. */
    struct input_absinfo ai;
    if (ioctl(in->fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0) {
        in->abs_x_max = (u32)ai.maximum;
    }
    if (ioctl(in->fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0) {
        in->abs_y_max = (u32)ai.maximum;
    }
    if (in->abs_x_max == 0) in->abs_x_max = 32767;
    if (in->abs_y_max == 0) in->abs_y_max = 32767;

    return VMC_OK;
}
