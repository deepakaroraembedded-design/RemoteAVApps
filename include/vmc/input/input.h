/*
 * input.h — input event model and capture abstraction.
 *
 * Events are normalized to a compact fixed-size record so the transport can
 * batch them cheaply (see batch.h). A capture backend (evdev on Linux,
 * I2C touch controller later) fills these in.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_INPUT_INPUT_H
#define VMC_INPUT_INPUT_H

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

typedef enum vmc_input_type {
    VMC_INPUT_TOUCH_DOWN = 0,
    VMC_INPUT_TOUCH_MOVE = 1,
    VMC_INPUT_TOUCH_UP   = 2,
    VMC_INPUT_BUTTON     = 3,
    VMC_INPUT_SENSOR     = 4,
    VMC_INPUT_COUNT
} vmc_input_type;

/* One normalized input event. 16 bytes — batch-friendly. */
typedef struct vmc_input_event {
    u16 type;     /* vmc_input_type */
    u16 slot;     /* touch slot / sensor id */
    u16 x;        /* normalized 0..65535 across panel width */
    u16 y;        /* normalized 0..65535 across panel height */
    u16 pressure; /* 0..1023 */
    u16 buttons;  /* bitmask (button events) */
    u32 ts_us;    /* local monotonic us */
} vmc_input_event;

/* Capture backend vtable. */
typedef struct vmc_input_capture vmc_input_capture;

typedef struct vmc_input_ops {
    vmc_status (*open)(vmc_input_capture *c);
    void       (*close)(vmc_input_capture *c);
    /* Poll one event; returns VMC_OK + fills ev, or VMC_ERR_AGAIN if none. */
    vmc_status (*poll)(vmc_input_capture *c, vmc_input_event *ev);
} vmc_input_ops;

struct vmc_input_capture {
    const vmc_input_ops *ops;
    void                *impl;
};

static inline vmc_status vmc_input_open(vmc_input_capture *c)  { return c->ops->open(c); }
static inline void       vmc_input_close(vmc_input_capture *c){ if (c->ops->close) c->ops->close(c); }
static inline vmc_status vmc_input_poll(vmc_input_capture *c, vmc_input_event *ev) {
    return c->ops->poll(c, ev);
}

VMC_END_DECLS

#endif /* VMC_INPUT_INPUT_H */
