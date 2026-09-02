/*
 * jitter_buffer.h — fixed-slot jitter buffer for ordered media delivery.
 *
 * Reorders out-of-order datagrams within a small window and holds them for a
 * target playout delay, smoothing network jitter for video/audio streams.
 * Caller-provided storage, RTOS-friendly (no allocation).
 *
 * Model: slots hold up to VMC_JB_MAX_SLOTS items each carrying a reference to
 * caller-provided buffer + metadata. Push with seq; pull in seq order once
 * oldest is old enough to play.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_TRANSPORT_JITTER_BUFFER_H
#define VMC_TRANSPORT_JITTER_BUFFER_H

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

#define VMC_JB_MAX_SLOTS 256

/* Each slot owns a copy of its payload. Sized for the largest supported
 * video fragment (1400 bytes + 4-byte fragment header + slack). */
#define VMC_JB_PAYLOAD_CAP 1408

typedef struct vmc_jb_slot {
    u32 seq;        /* wire sequence */
    u32 ts_us;      /* sender timestamp (low 32 bits) */
    u16 flags;      /* VMC_PROTO_FLAG_* */
    u8  stream;     /* stream type */
    u16 len;        /* payload bytes */
    u64 recv_us;    /* local monotonic time at push */
    const u8 *payload; /* owned copy (points into storage[]) */
    u8  storage[VMC_JB_PAYLOAD_CAP];
    bool valid;
} vmc_jb_slot;

typedef struct vmc_jitter_buffer {
    vmc_jb_slot slots[VMC_JB_MAX_SLOTS];
    u32 next_seq;       /* next expected seq for playout */
    u64 target_delay_us;/* desired playout delay (smoothing target) */
    u64 stats_pushed;
    u64 stats_played;
    u64 stats_dropped_late;   /* arrived after playout window */
    u64 stats_dropped_dupe;   /* duplicate seq */
    u64 stats_gaps;           /* playout gaps (lost in transit) */
} vmc_jitter_buffer;

vmc_status vmc_jb_init(vmc_jitter_buffer *jb, u64 target_delay_us);

/* Reset to empty; next playout accepts any new seq as the base. */
void vmc_jb_reset(vmc_jitter_buffer *jb, u32 first_seq);

/* Push one datagram's metadata into the buffer.
 * Returns VMC_OK, VMC_ERR_OVERRUN (slot full), VMC_ERR_INVALID_ARG. */
vmc_status vmc_jb_push(vmc_jitter_buffer *jb, u32 seq, u32 ts_us,
                       u8 stream, u16 flags, const u8 *payload, u16 len,
                       u64 recv_us);

/* Returns the next playable slot without removing it, or NULL if none yet
 * (not enough age, or empty). Advances internal state. */
const vmc_jb_slot *vmc_jb_peek(vmc_jitter_buffer *jb, u64 now_us);

/* Consume the slot returned by the last vmc_jb_peek(). */
void vmc_jb_consume(vmc_jitter_buffer *jb);

/* Drop everything before seq (e.g., after a seek/keyframe sync). */
void vmc_jb_flush_before(vmc_jitter_buffer *jb, u32 seq);

/* Live jitter estimate in us (simple EWMA of arrival deltas). */
u64 vmc_jb_estimate_jitter_us(const vmc_jitter_buffer *jb);

VMC_END_DECLS

#endif /* VMC_TRANSPORT_JITTER_BUFFER_H */
