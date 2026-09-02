#include "vmc/core/ringbuf.h"

#include <string.h>

#include "vmc/core/error.h"

static inline sz_t rb_mask(const vmc_ringbuf *rb, sz_t idx) {
    return idx & rb->mask;
}

static inline bool is_pow2_or_zero(sz_t n) {
    return n && (n & (n - 1)) == 0;
}

vmc_status vmc_ringbuf_init(vmc_ringbuf *rb, void *storage, sz_t capacity) {
    if (!rb || !storage || !is_pow2_or_zero(capacity)) {
        return VMC_ERR_INVALID_ARG;
    }
    rb->data  = (u8 *)storage;
    rb->mask  = capacity - 1;
    rb->head  = 0;
    rb->tail  = 0;
    rb->count = 0;
    return VMC_OK;
}

void vmc_ringbuf_reset(vmc_ringbuf *rb) {
    rb->head  = 0;
    rb->tail  = 0;
    rb->count = 0;
}

sz_t vmc_ringbuf_capacity(const vmc_ringbuf *rb) { return rb->mask + 1; }
sz_t vmc_ringbuf_used(const vmc_ringbuf *rb)     { return rb->count; }
sz_t vmc_ringbuf_free(const vmc_ringbuf *rb)     { return vmc_ringbuf_capacity(rb) - rb->count; }
bool vmc_ringbuf_empty(const vmc_ringbuf *rb)    { return rb->count == 0; }
bool vmc_ringbuf_full(const vmc_ringbuf *rb)     { return rb->count == vmc_ringbuf_capacity(rb); }

sz_t vmc_ringbuf_write(vmc_ringbuf *rb, const void *data, sz_t len) {
    if (len == 0) return 0;
    if (len > vmc_ringbuf_free(rb)) {
        len = vmc_ringbuf_free(rb);
    }
    const u8 *src = (const u8 *)data;
    const sz_t cap = vmc_ringbuf_capacity(rb);
    const sz_t first = cap - rb_mask(rb, rb->head);
    const sz_t n1 = len < first ? len : first;
    memcpy(&rb->data[rb_mask(rb, rb->head)], src, n1);
    if (len > n1) {
        memcpy(&rb->data[0], src + n1, len - n1);
    }
    rb->head += len;
    rb->count += len;
    return len;
}

sz_t vmc_ringbuf_read(vmc_ringbuf *rb, void *out, sz_t len) {
    if (len > rb->count) {
        len = rb->count;
    }
    if (len == 0) return 0;
    const sz_t cap = vmc_ringbuf_capacity(rb);
    const sz_t first = cap - rb_mask(rb, rb->tail);
    const sz_t n1 = len < first ? len : first;
    if (out) {
        memcpy(out, &rb->data[rb_mask(rb, rb->tail)], n1);
        if (len > n1) {
            memcpy((u8 *)out + n1, &rb->data[0], len - n1);
        }
    }
    rb->tail += len;
    rb->count -= len;
    return len;
}

sz_t vmc_ringbuf_peek(const vmc_ringbuf *rb, void *out, sz_t len) {
    if (len > rb->count) {
        len = rb->count;
    }
    if (len == 0) return 0;
    const sz_t cap = vmc_ringbuf_capacity(rb);
    const sz_t first = cap - rb_mask(rb, rb->tail);
    const sz_t n1 = len < first ? len : first;
    memcpy(out, &rb->data[rb_mask(rb, rb->tail)], n1);
    if (len > n1) {
        memcpy((u8 *)out + n1, &rb->data[0], len - n1);
    }
    return len;
}

sz_t vmc_ringbuf_discard(vmc_ringbuf *rb, sz_t len) {
    if (len > rb->count) {
        len = rb->count;
    }
    rb->tail += len;
    rb->count -= len;
    return len;
}
