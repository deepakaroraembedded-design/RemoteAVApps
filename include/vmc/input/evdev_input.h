/*
 * evdev_input.h — Linux evdev touch/sensor capture backend.
 *
 * Reads ABS_MT_* multi-touch events from /dev/input/eventN and normalizes
 * to vmc_input_event. Linux/embedded targets only.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_INPUT_EVDEV_INPUT_H
#define VMC_INPUT_EVDEV_INPUT_H

#include <stddef.h>

#include "vmc/core/types.h"
#include "vmc/input/input.h"

VMC_BEGIN_DECLS

typedef struct vmc_evdev_input {
    vmc_input_capture base;  /* must be first */
    int               fd;
    u32               abs_x_max;  /* device ABS_MT_POSITION_X max */
    u32               abs_y_max;  /* device ABS_MT_POSITION_Y max */
} vmc_evdev_input;

/* device_path e.g. "/dev/input/event1"; NULL -> try to auto-find a touch
 * device. abs_max values default to 32767 if 0. */
vmc_status vmc_evdev_init(vmc_evdev_input *in, const char *device_path);

VMC_END_DECLS

#endif /* VMC_INPUT_EVDEV_INPUT_H */
