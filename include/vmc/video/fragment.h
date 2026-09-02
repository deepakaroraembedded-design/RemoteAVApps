/*
 * fragment.h — video access-unit fragmentation (wire format + assembler).
 *
 * Encoded frames (access units) are larger than the protocol datagram cap,
 * so the MEC splits each AU into chunks. Every chunk is carried in its own
 * protocol datagram with VMC_PROTO_FLAG_FRAGMENTED set, and the payload
 * begins with a 4-byte little-endian fragment header:
 *
 *   off 0  u16 frame_id    — identifies the AU (set to the AU's first seq)
 *   off 2  u16 frag_index  — chunk index; bit 15 (0x8000) = LAST chunk
 *
 * Chunks are 1400 bytes (MTU-safe). Unfragmented AUs are sent whole with
 * the FRAGMENTED flag clear and no fragment header.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_VIDEO_FRAGMENT_H
#define VMC_VIDEO_FRAGMENT_H

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

#define VMC_VIDEO_FRAG_CHUNK    1400u
#define VMC_VIDEO_FRAG_LAST     0x8000u
#define VMC_VIDEO_FRAG_INDEX_MASK 0x7FFFu
/* Large enough for a high-bitrate 4K keyframe. */
#define VMC_VIDEO_AU_MAX        (2u * 1024u * 1024u)

/* Serialize/parse the 4-byte fragment header. */
void vmc_video_frag_hdr_pack(u8 *out, u16 frame_id, u16 frag_index);
void vmc_video_frag_hdr_unpack(const u8 *in, u16 *frame_id, u16 *frag_index,
                               bool *last);

typedef struct vmc_frag_assembler {
    u8  *buf;           /* caller-owned AU buffer */
    sz_t cap;           /* capacity (<= VMC_VIDEO_AU_MAX) */
    u16  frame_id;      /* current frame being assembled */
    u16  next_index;    /* next expected chunk index */
    sz_t filled;        /* bytes written so far */
    bool started;
} vmc_frag_assembler;

vmc_status vmc_frag_init(vmc_frag_assembler *a, u8 *buf, sz_t cap);

/* Feed one chunk (payload WITHOUT the 4-byte frag header; header passed in).
 * On success returns VMC_OK and sets *out_au_len to the AU length when the
 * frame is complete (0 while collecting). Returns VMC_ERR_OUT_OF_SYNC if a
 * chunk for a new frame arrives (assembler auto-resets for it) or an index
 * is missing. */
vmc_status vmc_frag_feed(vmc_frag_assembler *a, u16 frame_id, u16 frag_index,
                         bool last, const u8 *chunk, sz_t len,
                         sz_t *out_au_len);

void vmc_frag_reset(vmc_frag_assembler *a);

VMC_END_DECLS

#endif /* VMC_VIDEO_FRAGMENT_H */
