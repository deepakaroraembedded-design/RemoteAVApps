/*
 * transport.h — transport abstraction for the thin client.
 *
 * A transport owns a bidirectional, packet-oriented channel to the MEC
 * container or mapper. Implementations: udp_transport.h (Linux), and later
 * 5G-modem/QUIC backends. All APIs are non-blocking.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_TRANSPORT_TRANSPORT_H
#define VMC_TRANSPORT_TRANSPORT_H

#include "vmc/core/types.h"
#include "vmc/transport/protocol.h"

VMC_BEGIN_DECLS

typedef struct vmc_transport vmc_transport;

typedef struct vmc_transport_ops {
    vmc_status (*open)(vmc_transport *t);
    void       (*close)(vmc_transport *t);

    /* Send one full protocol packet (header+payload, as produced by
     * vmc_proto_encode). Non-blocking; VMC_ERR_AGAIN if send buffer full. */
    vmc_status (*send)(vmc_transport *t, const u8 *packet, sz_t len);

    /* Receive one datagram into buf (capacity *cap); on success returns
     * VMC_OK and sets *out_len. Returns VMC_ERR_AGAIN when no data. */
    vmc_status (*recv)(vmc_transport *t, u8 *buf, sz_t cap, sz_t *out_len);

    /* Transport-level stats snapshot. */
    void (*stats)(const vmc_transport *t, u64 *tx_bytes, u64 *rx_bytes,
                  u64 *tx_packets, u64 *rx_packets);
} vmc_transport_ops;

struct vmc_transport {
    const vmc_transport_ops *ops;
    void                    *impl;
    /* Common stats kept by helpers in transport.c if implementations use
     * vmc_transport_common_* helpers. */
    u64 tx_bytes;
    u64 rx_bytes;
    u64 tx_packets;
    u64 rx_packets;
};

static inline vmc_status vmc_transport_open(vmc_transport *t) {
    return t->ops->open(t);
}
static inline void vmc_transport_close(vmc_transport *t) {
    if (t->ops->close) t->ops->close(t);
}
static inline vmc_status vmc_transport_send(vmc_transport *t, const u8 *p, sz_t len) {
    return t->ops->send(t, p, len);
}
static inline vmc_status vmc_transport_recv(vmc_transport *t, u8 *buf, sz_t cap, sz_t *out) {
    return t->ops->recv(t, buf, cap, out);
}

/* Helpers for implementations to keep shared stats coherent. */
void vmc_transport_note_tx(vmc_transport *t, sz_t bytes);
void vmc_transport_note_rx(vmc_transport *t, sz_t bytes);

VMC_END_DECLS

#endif /* VMC_TRANSPORT_TRANSPORT_H */
