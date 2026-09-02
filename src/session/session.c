#include "vmc/session/session.h"

#include <string.h>

#include "vmc/core/error.h"
#include "vmc/core/logger.h"
#include "vmc/core/platform.h"
#include "vmc/transport/protocol.h"

vmc_status vmc_session_ctx_init(vmc_session_ctx *sc, vmc_transport *t,
                                const vmc_session_config *cfg, void *user) {
    if (!sc || !t || !cfg) return VMC_ERR_INVALID_ARG;

    memset(sc, 0, sizeof(*sc));
    sc->transport = t;
    sc->cfg = *cfg;
    sc->keepalive_interval_ms = cfg->keepalive_ms ? cfg->keepalive_ms : 2000u;
    /* now_ms is boot-relative monotonic time; anchor the "last seen"
     * timestamps to now so the first step() doesn't misread them as
     * expired. */
    sc->last_rx_ms = vmc_time_now_ms();
    sc->last_keepalive_ms = sc->last_rx_ms;

    vmc_session_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    return vmc_session_init(&sc->sm, cb, user);
}

vmc_status vmc_session_start(vmc_session_ctx *sc) {
    if (!sc) return VMC_ERR_INVALID_ARG;
    return vmc_session_dispatch(&sc->sm, VMC_EVENT_BOOT);
}

vmc_status vmc_session_stop(vmc_session_ctx *sc) {
    if (!sc) return VMC_ERR_INVALID_ARG;
    return vmc_session_dispatch(&sc->sm, VMC_EVENT_USER_TERMINATE);
}

static vmc_status send_keepalive(vmc_session_ctx *sc, u64 now_ms) {
    u8 pkt[VMC_PROTO_HEADER_SIZE];
    vmc_proto_header h;
    memset(&h, 0, sizeof(h));
    h.magic       = VMC_PROTO_MAGIC;
    h.version     = VMC_PROTO_VERSION;
    h.stream      = (u8)VMC_PROTO_STREAM_CONTROL;
    h.payload_len = 0;
    h.seq         = (u32)sc->tx_seq++;
    h.ts_us       = (u32)now_ms;
    vmc_status st = vmc_proto_encode(pkt, sizeof(pkt), &h, NULL);
    if (st < 0) return st;
    sc->last_ka_send_us = vmc_time_now_us();
    return vmc_transport_send(sc->transport, pkt, (sz_t)st);
}

vmc_status vmc_session_step(vmc_session_ctx *sc, u64 now_ms) {
    if (!sc) return VMC_ERR_INVALID_ARG;

    switch (vmc_session_get_state(&sc->sm)) {
        case VMC_SESSION_ACTIVE:
        case VMC_SESSION_DEGRADED:
            /* Keepalive */
            if (now_ms - sc->last_keepalive_ms >= sc->keepalive_interval_ms) {
                (void)send_keepalive(sc, now_ms);
                sc->last_keepalive_ms = now_ms;
            }
            /* Link loss detection */
            if (sc->cfg.link_timeout_ms > 0 &&
                now_ms - sc->last_rx_ms > sc->cfg.link_timeout_ms) {
                VMC_LOGW("session: link loss (%u ms without traffic)",
                         sc->cfg.link_timeout_ms);
                return vmc_session_dispatch(&sc->sm, VMC_EVENT_LINK_LOST);
            }
            return VMC_OK;

        default:
            return VMC_OK;
    }
}

vmc_status vmc_session_on_rx(vmc_session_ctx *sc, const u8 *packet, sz_t len) {
    if (!sc || !packet) return VMC_ERR_INVALID_ARG;

    vmc_proto_header h;
    vmc_status st = vmc_proto_decode_header(packet, len, &h);
    if (st != VMC_OK) {
        return st;
    }
    if (h.stream == VMC_PROTO_STREAM_CONTROL) {
        sc->last_rx_ms = vmc_time_now_ms();
    }
    return VMC_OK;
}
