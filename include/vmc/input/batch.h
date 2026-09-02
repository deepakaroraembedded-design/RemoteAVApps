/*
 * batch.h — input event batching.
 *
 * Input events are accumulated and flushed at a fixed cadence (e.g. every
 * 4 ms / 250 Hz per the latency budget) into one transport payload, cutting
 * per-event uplink overhead.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_INPUT_BATCH_H
#define VMC_INPUT_BATCH_H

#include "vmc/core/types.h"
#include "vmc/input/input.h"

VMC_BEGIN_DECLS

#define VMC_INPUT_BATCH_MAX_EVENTS 32

typedef struct vmc_input_batch {
    vmc_input_event events[VMC_INPUT_BATCH_MAX_EVENTS];
    u16             count;
} vmc_input_batch;

void       vmc_input_batch_reset(vmc_input_batch *b);
bool       vmc_input_batch_full(const vmc_input_batch *b);
/* Returns VMC_OK, VMC_ERR_OVERRUN if full. */
vmc_status vmc_input_batch_push(vmc_input_batch *b, const vmc_input_event *ev);

/* Wire format: u16 count, then count * sizeof(vmc_input_event) (LE).
 * Returns bytes written or negative status. */
vmc_status vmc_input_batch_serialize(const vmc_input_batch *b, u8 *out, sz_t cap);

/* Inverse of serialize. */
vmc_status vmc_input_batch_deserialize(const u8 *data, sz_t len, vmc_input_batch *out);

VMC_END_DECLS

#endif /* VMC_INPUT_BATCH_H */
