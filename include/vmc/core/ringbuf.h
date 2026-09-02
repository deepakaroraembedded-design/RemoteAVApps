/*
 * ringbuf.h — fixed-capacity byte ring buffer.
 *
 * RTOS-friendly: no allocation, caller provides storage. All capacity limits
 * are powers of two so indices wrap by masking (no modulo in the hot path).
 * Single-producer/single-consumer safe when used from two contexts with
 * a memory barrier between produce and consume (see notes in ringbuf.c).
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_CORE_RINGBUF_H
#define VMC_CORE_RINGBUF_H

#include <stddef.h>
#include <stdint.h>

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

typedef struct vmc_ringbuf {
    u8  *data;
    sz_t mask;    /* capacity - 1; capacity must be a power of two */
    sz_t head;    /* write offset (mod capacity) */
    sz_t tail;    /* read offset (mod capacity) */
    sz_t count;   /* bytes currently stored */
} vmc_ringbuf;

/* capacity must be a power of two and >= 1. Returns VMC_ERR_INVALID_ARG else. */
vmc_status vmc_ringbuf_init(vmc_ringbuf *rb, void *storage, sz_t capacity);

void      vmc_ringbuf_reset(vmc_ringbuf *rb);

sz_t      vmc_ringbuf_capacity(const vmc_ringbuf *rb);
sz_t      vmc_ringbuf_used(const vmc_ringbuf *rb);
sz_t      vmc_ringbuf_free(const vmc_ringbuf *rb);
bool      vmc_ringbuf_empty(const vmc_ringbuf *rb);
bool      vmc_ringbuf_full(const vmc_ringbuf *rb);

/* Returns bytes written; 0 if not enough contiguous/disc space yet.
 * never exceeds len. May write fewer than len before capacity is full. */
sz_t vmc_ringbuf_write(vmc_ringbuf *rb, const void *data, sz_t len);

/* Returns bytes read; 0 if empty. */
sz_t vmc_ringbuf_read(vmc_ringbuf *rb, void *out, sz_t len);

/* Peek without consuming. */
sz_t vmc_ringbuf_peek(const vmc_ringbuf *rb, void *out, sz_t len);

/* Skip len bytes without copying. */
sz_t vmc_ringbuf_discard(vmc_ringbuf *rb, sz_t len);

VMC_END_DECLS

#endif /* VMC_CORE_RINGBUF_H */
