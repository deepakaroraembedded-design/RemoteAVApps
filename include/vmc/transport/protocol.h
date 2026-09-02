/*
 * protocol.h — VMC thin-client wire protocol.
 *
 * One fixed-size header + payload per datagram (UDP media plane, or framed
 * over the TCP/Unix control plane). All multi-byte fields are little-endian
 * on the wire regardless of host.
 *
 * Header layout (17 bytes):
 *   off 0   u16 magic        = 0x5643 ("VC")
 *   off 2   u8  version      = 1
 *   off 3   u8  flags        (VMC_PROTO_FLAG_*)
 *   off 4   u8  stream       (VMC_PROTO_STREAM_*)
 *   off 5   u8  reserved     = 0
 *   off 6   u16 payload_len  [0..65535]
 *   off 8   u32 seq          (per-stream, wraps)
 *   off 12  u32 ts_us        (low 32 bits of monotonic us)
 *   off 16  u8  crc8         over header[0..15] + payload
 *   off 17  u8  payload[]
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_TRANSPORT_PROTOCOL_H
#define VMC_TRANSPORT_PROTOCOL_H

#include <stddef.h>

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

#define VMC_PROTO_MAGIC        0x5643u
#define VMC_PROTO_VERSION      1u
#define VMC_PROTO_HEADER_SIZE  17u
#define VMC_PROTO_MAX_PAYLOAD  65535u
#define VMC_PROTO_MAX_PACKET   (VMC_PROTO_HEADER_SIZE + VMC_PROTO_MAX_PAYLOAD)

/* Stream type classification. */
typedef enum vmc_proto_stream {
    VMC_PROTO_STREAM_CONTROL   = 0,  /* session, keepalive, degradation, auth */
    VMC_PROTO_STREAM_VIDEO     = 1,  /* encoded video from MEC */
    VMC_PROTO_STREAM_AUDIO     = 2,  /* MEC audio downlink */
    VMC_PROTO_STREAM_INPUT     = 3,  /* touch/sensor/mic uplink to MEC */
    VMC_PROTO_STREAM_TELEMETRY = 4,  /* rtt, rssi, qos reports */
    VMC_PROTO_STREAM_COUNT
} vmc_proto_stream;

/* Header flags. */
enum {
    VMC_PROTO_FLAG_FEC        = 1u << 0, /* payload carries FEC parity data */
    VMC_PROTO_FLAG_KEYFRAME   = 1u << 1, /* video frame is a keyframe */
    VMC_PROTO_FLAG_FRAGMENTED = 1u << 2, /* part of a multi-datagram frame */
};

/* Decoded, host-order header. */
typedef struct vmc_proto_header {
    u16    magic;
    u8     version;
    u8     flags;
    u8     stream;
    u8     reserved;
    u16    payload_len;
    u32    seq;
    u32    ts_us;
    u8     crc;
} vmc_proto_header;

/* CRC-8 (poly 0x07, init 0x00). Exposed for tooling and tests. */
u8 vmc_proto_crc8(const void *data, sz_t len);

/* Validate a raw header (magic, version, reserved). Payload NOT checked. */
bool vmc_proto_header_valid(const vmc_proto_header *h);

/* Serialize header + payload into buf. buf must hold >= header_size+payload_len.
 * Returns bytes written, or a negative vmc_status. */
vmc_status vmc_proto_encode(u8 *buf, sz_t buf_cap,
                            const vmc_proto_header *h, const void *payload);

/* Decode and validate a full datagram (header + crc over header+payload).
 * Returns VMC_OK, VMC_ERR_CORRUPT, VMC_ERR_PROTO, or VMC_ERR_INVALID_ARG. */
vmc_status vmc_proto_decode(const u8 *buf, sz_t len,
                            vmc_proto_header *out_h, const u8 **out_payload);

/* Convenience decode of header only (no payload crc verification). */
vmc_status vmc_proto_decode_header(const u8 *buf, sz_t len, vmc_proto_header *out_h);

VMC_END_DECLS

#endif /* VMC_TRANSPORT_PROTOCOL_H */
