/*
 * session.h — thin client session object.
 *
 * Owns the lifecycle state machine and the active media transport, and drives
 * both from a single non-blocking step() call — RTOS-loop friendly.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_SESSION_SESSION_H
#define VMC_SESSION_SESSION_H

#include "vmc/core/types.h"
#include "vmc/session/state.h"
#include "vmc/transport/transport.h"

VMC_BEGIN_DECLS

#define VMC_SESSION_MAX_ROUTE 128

typedef struct vmc_session_config {
    char container_host[VMC_SESSION_MAX_ROUTE]; /* mapper-provided route */
    u16  container_port;
    u32  keepalive_ms;   /* control-plane keepalive interval */
    u32  link_timeout_ms;/* no-traffic => LINK_LOST */
} vmc_session_config;

typedef struct vmc_session_ctx {
    vmc_session         sm;       /* state machine */
    vmc_transport      *transport;/* media transport (borrowed) */
    vmc_session_config  cfg;
    u64                 last_rx_ms;
    u64                 last_keepalive_ms;
    u64                 last_ka_send_us;   /* monotonic us, last keepalive tx */
    u64                 tx_seq;
    u32                 keepalive_interval_ms;
} vmc_session_ctx;

vmc_status vmc_session_ctx_init(vmc_session_ctx *sc, vmc_transport *t,
                                const vmc_session_config *cfg, void *user);

/* Drive the session: sends keepalives, detects link loss, returns
 * VMC_OK when healthy. */
vmc_status vmc_session_step(vmc_session_ctx *sc, u64 now_ms);

vmc_status vmc_session_start(vmc_session_ctx *sc);
vmc_status vmc_session_stop(vmc_session_ctx *sc);

/* Deliver a received packet to the session (e.g., control-plane ack). */
vmc_status vmc_session_on_rx(vmc_session_ctx *sc, const u8 *packet, sz_t len);

VMC_END_DECLS

#endif /* VMC_SESSION_SESSION_H */
