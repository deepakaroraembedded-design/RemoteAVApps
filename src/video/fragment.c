#include "vmc/video/fragment.h"

#include <string.h>

#include "vmc/core/error.h"

void vmc_video_frag_hdr_pack(u8 *out, u16 frame_id, u16 frag_index) {
    out[0] = (u8)(frame_id & 0xffu);
    out[1] = (u8)((frame_id >> 8) & 0xffu);
    out[2] = (u8)(frag_index & 0xffu);
    out[3] = (u8)((frag_index >> 8) & 0xffu);
}

void vmc_video_frag_hdr_unpack(const u8 *in, u16 *frame_id, u16 *frag_index,
                               bool *last) {
    const u16 idx = (u16)((u16)in[2] | ((u16)in[3] << 8));
    if (frame_id)  { *frame_id  = (u16)((u16)in[0] | ((u16)in[1] << 8)); }
    if (frag_index){ *frag_index = idx & VMC_VIDEO_FRAG_INDEX_MASK; }
    if (last)      { *last      = (idx & VMC_VIDEO_FRAG_LAST) != 0; }
}

vmc_status vmc_frag_init(vmc_frag_assembler *a, u8 *buf, sz_t cap) {
    if (!a || !buf || cap == 0 || cap > VMC_VIDEO_AU_MAX) {
        return VMC_ERR_INVALID_ARG;
    }
    a->buf  = buf;
    a->cap  = cap;
    vmc_frag_reset(a);
    return VMC_OK;
}

void vmc_frag_reset(vmc_frag_assembler *a) {
    a->frame_id   = 0;
    a->next_index = 0;
    a->filled     = 0;
    a->started    = false;
}

vmc_status vmc_frag_feed(vmc_frag_assembler *a, u16 frame_id, u16 frag_index,
                         bool last, const u8 *chunk, sz_t len,
                         sz_t *out_au_len) {
    if (out_au_len) *out_au_len = 0;
    if (!a || (len > 0 && !chunk)) return VMC_ERR_INVALID_ARG;

    /* First chunk of a frame starts a new assembly. */
    if (!a->started || frame_id != a->frame_id) {
        a->frame_id   = frame_id;
        a->next_index = 0;
        a->filled     = 0;
        a->started    = true;
    }

    /* Missing chunk index within the current frame: drop it. */
    if (frag_index != a->next_index) {
        a->started = false;
        return VMC_ERR_OUT_OF_SYNC;
    }

    if (len > a->cap - a->filled) {
        a->started = false;
        return VMC_ERR_OVERRUN;
    }

    memcpy(a->buf + a->filled, chunk, len);
    a->filled += len;
    a->next_index++;

    if (last) {
        if (out_au_len) *out_au_len = a->filled;
        a->started = false;
        return VMC_OK;
    }
    return VMC_OK;
}
