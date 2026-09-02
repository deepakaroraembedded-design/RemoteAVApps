#include "vmc/input/batch.h"

#include <string.h>

#include "vmc/core/compiler.h"
#include "vmc/core/error.h"

void vmc_input_batch_reset(vmc_input_batch *b) {
    b->count = 0;
}

bool vmc_input_batch_full(const vmc_input_batch *b) {
    return b->count >= VMC_INPUT_BATCH_MAX_EVENTS;
}

vmc_status vmc_input_batch_push(vmc_input_batch *b, const vmc_input_event *ev) {
    if (!b || !ev) return VMC_ERR_INVALID_ARG;
    if (vmc_input_batch_full(b)) return VMC_ERR_OVERRUN;
    b->events[b->count] = *ev;
    b->count++;
    return VMC_OK;
}

/* All multi-byte fields of vmc_input_event are written little-endian. */
static inline void w16(u8 *p, u16 v) { p[0] = (u8)(v & 0xffu); p[1] = (u8)((v >> 8) & 0xffu); }
static inline void w32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xffu); p[1] = (u8)((v >> 8) & 0xffu);
    p[2] = (u8)((v >> 16) & 0xffu); p[3] = (u8)((v >> 24) & 0xffu);
}
static inline u16 r16(const u8 *p) { return (u16)((u16)p[0] | ((u16)p[1] << 8)); }
static inline u32 r32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

vmc_status vmc_input_batch_serialize(const vmc_input_batch *b, u8 *out, sz_t cap) {
    if (!b || !out) return VMC_ERR_INVALID_ARG;
    const sz_t need = 2u + (sz_t)b->count * sizeof(vmc_input_event);
    if (cap < need) return VMC_ERR_NOMEM;

    w16(out, b->count);
    u8 *p = out + 2;
    for (u16 i = 0; i < b->count; i++) {
        const vmc_input_event *e = &b->events[i];
        w16(p + 0,  e->type);
        w16(p + 2,  e->slot);
        w16(p + 4,  e->x);
        w16(p + 6,  e->y);
        w16(p + 8,  e->pressure);
        w16(p + 10, e->buttons);
        w32(p + 12, e->ts_us);
        p += sizeof(vmc_input_event);
    }
    return (vmc_status)need;
}

vmc_status vmc_input_batch_deserialize(const u8 *data, sz_t len, vmc_input_batch *out) {
    if (!data || !out) return VMC_ERR_INVALID_ARG;
    if (len < 2) return VMC_ERR_PROTO;

    const u16 count = r16(data);
    const sz_t need = 2u + (sz_t)count * sizeof(vmc_input_event);
    if (need > len) return VMC_ERR_PROTO;
    if (count > VMC_INPUT_BATCH_MAX_EVENTS) return VMC_ERR_PROTO;

    out->count = count;
    const u8 *p = data + 2;
    for (u16 i = 0; i < count; i++) {
        vmc_input_event *e = &out->events[i];
        e->type     = r16(p + 0);
        e->slot     = r16(p + 2);
        e->x        = r16(p + 4);
        e->y        = r16(p + 6);
        e->pressure = r16(p + 8);
        e->buttons  = r16(p + 10);
        e->ts_us    = r32(p + 12);
        p += sizeof(vmc_input_event);
    }
    return VMC_OK;
}
