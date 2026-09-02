/*
 * udp_transport.h — Linux UDP implementation of vmc_transport.
 *
 * Suitable for LAN/5G media plane (RTP-style best-effort). Caller provides
 * the socket (or address), transport binds/connects it.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_TRANSPORT_UDP_TRANSPORT_H
#define VMC_TRANSPORT_UDP_TRANSPORT_H

#include <stddef.h>

#include "vmc/core/types.h"
#include "vmc/transport/transport.h"

VMC_BEGIN_DECLS

typedef struct vmc_udp_transport {
    vmc_transport base;      /* must be first */
    int           fd;        /* -1 when closed */
    u8           *recv_buf;  /* caller storage; VMC_UDP_RECV_BUF default */
    sz_t          recv_cap;
    u16           local_port; /* bound port (0 = ephemeral) */
} vmc_udp_transport;

#define VMC_UDP_RECV_BUF_DEFAULT 2048

/* Initialize. recv_buf may be NULL to use a static internal buffer. */
vmc_status vmc_udp_init(vmc_udp_transport *u, u8 *recv_buf, sz_t recv_cap);

/* Bind+connect to remote "host:port" (host may be IPv4/IPv6 literal or name).
 * Returns VMC_OK or VMC_ERR_IO. */
vmc_status vmc_udp_connect(vmc_udp_transport *u, const char *host, u16 port);

VMC_END_DECLS

#endif /* VMC_TRANSPORT_UDP_TRANSPORT_H */
