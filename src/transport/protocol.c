#include "vmc/transport/protocol.h"

#include <string.h>

#include "vmc/core/compiler.h"
#include "vmc/core/error.h"

static inline u16 rd16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static inline u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline void wr16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xffu);
    p[1] = (u8)((v >> 8) & 0xffu);
}

static inline void wr32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xffu);
    p[1] = (u8)((v >> 8) & 0xffu);
    p[2] = (u8)((v >> 16) & 0xffu);
    p[3] = (u8)((v >> 24) & 0xffu);
}

/* CRC-8 continuation: feed bytes into an in-progress CRC. */
static u8 crc8_update(u8 crc, const u8 *data, sz_t len) {
    sz_t i;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80u) {
                crc = (u8)(((unsigned)crc << 1) ^ 0x07u);
            } else {
                crc = (u8)((unsigned)crc << 1);
            }
        }
    }
    return crc;
}

u8 vmc_proto_crc8(const void *data, sz_t len) {
    return crc8_update(0, (const u8 *)data, len);
}

/* CRC over the wire framing: 16-byte header prefix followed by payload. */
static u8 frame_crc(const u8 *prefix, sz_t prefix_len,
                    const u8 *payload, sz_t payload_len) {
    u8 crc = crc8_update(0, prefix, prefix_len);
    if (payload && payload_len > 0) {
        crc = crc8_update(crc, payload, payload_len);
    }
    return crc;
}

bool vmc_proto_header_valid(const vmc_proto_header *h) {
    if (!h) return false;
    if (h->magic != VMC_PROTO_MAGIC) return false;
    if (h->version != VMC_PROTO_VERSION) return false;
    if (h->reserved != 0) return false;
    return true;
}

vmc_status vmc_proto_encode(u8 *buf, sz_t buf_cap,
                            const vmc_proto_header *h, const void *payload) {
    sz_t total;
    if (!buf || !h || !vmc_proto_header_valid(h)) {
        return VMC_ERR_INVALID_ARG;
    }
    total = (sz_t)VMC_PROTO_HEADER_SIZE + h->payload_len;
    if (buf_cap < total) {
        return VMC_ERR_NOMEM;
    }
    wr16(buf + 0, h->magic);
    buf[2] = h->version;
    buf[3] = h->flags;
    buf[4] = h->stream;
    buf[5] = h->reserved;
    wr16(buf + 6, h->payload_len);
    wr32(buf + 8, h->seq);
    wr32(buf + 12, h->ts_us);
    buf[16] = 0; /* placeholder */

    if (h->payload_len > 0 && payload) {
        memmove(buf + VMC_PROTO_HEADER_SIZE, payload, h->payload_len);
    }
    buf[16] = frame_crc(buf, VMC_PROTO_HEADER_SIZE - 1,
                        payload, h->payload_len);
    return (vmc_status)total;
}

vmc_status vmc_proto_decode_header(const u8 *buf, sz_t len, vmc_proto_header *out_h) {
    if (!buf || !out_h || len < VMC_PROTO_HEADER_SIZE) {
        return VMC_ERR_INVALID_ARG;
    }
    vmc_proto_header h;
    h.magic      = rd16(buf + 0);
    h.version    = buf[2];
    h.flags      = buf[3];
    h.stream     = buf[4];
    h.reserved   = buf[5];
    h.payload_len = rd16(buf + 6);
    h.seq        = rd32(buf + 8);
    h.ts_us      = rd32(buf + 12);
    h.crc        = buf[16];

    if (!vmc_proto_header_valid(&h)) {
        return VMC_ERR_PROTO;
    }
    *out_h = h;
    return VMC_OK;
}

vmc_status vmc_proto_decode(const u8 *buf, sz_t len,
                            vmc_proto_header *out_h, const u8 **out_payload) {
    vmc_proto_header h;
    vmc_status st = vmc_proto_decode_header(buf, len, &h);
    if (st != VMC_OK) {
        return st;
    }
    if (len < (sz_t)VMC_PROTO_HEADER_SIZE + h.payload_len) {
        return VMC_ERR_PROTO;
    }
    if (h.crc != frame_crc(buf, VMC_PROTO_HEADER_SIZE - 1,
                           buf + VMC_PROTO_HEADER_SIZE, h.payload_len)) {
        return VMC_ERR_CORRUPT;
    }
    if (out_h)      { *out_h = h; }
    if (out_payload) { *out_payload = buf + VMC_PROTO_HEADER_SIZE; }
    return VMC_OK;
}
