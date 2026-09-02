#include "vmc/transport/udp_transport.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>

#include "vmc/core/error.h"
#include "vmc/core/logger.h"

static vmc_status udp_open(vmc_transport *t) {
    vmc_udp_transport *u = (vmc_udp_transport *)t;
    if (u->fd >= 0) {
        return VMC_OK; /* already open */
    }
    u->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (u->fd < 0) {
        VMC_LOGE("udp_open: socket failed: %s", strerror(errno));
        return VMC_ERR_IO;
    }
    return VMC_OK;
}

static void udp_close(vmc_transport *t) {
    vmc_udp_transport *u = (vmc_udp_transport *)t;
    if (u->fd >= 0) {
        (void)close(u->fd);
        u->fd = -1;
    }
}

static vmc_status udp_send(vmc_transport *t, const u8 *packet, sz_t len) {
    vmc_udp_transport *u = (vmc_udp_transport *)t;
    if (u->fd < 0) {
        return VMC_ERR_NOT_CONNECTED;
    }
    ssize_t n = send(u->fd, packet, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return VMC_ERR_AGAIN;
        }
        VMC_LOGW("udp_send: send failed: %s", strerror(errno));
        return VMC_ERR_IO;
    }
    vmc_transport_note_tx(t, (sz_t)n);
    return VMC_OK;
}

static vmc_status udp_recv(vmc_transport *t, u8 *buf, sz_t cap, sz_t *out_len) {
    vmc_udp_transport *u = (vmc_udp_transport *)t;
    if (u->fd < 0) {
        return VMC_ERR_NOT_CONNECTED;
    }
    ssize_t n = recv(u->fd, buf, cap, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return VMC_ERR_AGAIN;
        }
        return VMC_ERR_IO;
    }
    if (n == 0) {
        return VMC_ERR_AGAIN;
    }
    if (out_len) {
        *out_len = (sz_t)n;
    }
    vmc_transport_note_rx(t, (sz_t)n);
    return VMC_OK;
}

static void udp_stats(const vmc_transport *t, u64 *tx_bytes, u64 *rx_bytes,
                      u64 *tx_packets, u64 *rx_packets) {
    if (tx_bytes)   *tx_bytes   = t->tx_bytes;
    if (rx_bytes)   *rx_bytes   = t->rx_bytes;
    if (tx_packets) *tx_packets = t->tx_packets;
    if (rx_packets) *rx_packets = t->rx_packets;
}

static const vmc_transport_ops k_udp_ops = {
    .open  = udp_open,
    .close = udp_close,
    .send  = udp_send,
    .recv  = udp_recv,
    .stats = udp_stats,
};

static u8 g_default_recv_buf[VMC_UDP_RECV_BUF_DEFAULT];

vmc_status vmc_udp_init(vmc_udp_transport *u, u8 *recv_buf, sz_t recv_cap) {
    if (!u) return VMC_ERR_INVALID_ARG;
    memset(u, 0, sizeof(*u));
    u->base.ops = &k_udp_ops;
    u->fd = -1;
    if (recv_buf && recv_cap > 0) {
        u->recv_buf = recv_buf;
        u->recv_cap = recv_cap;
    } else {
        u->recv_buf = g_default_recv_buf;
        u->recv_cap = sizeof(g_default_recv_buf);
    }
    return VMC_OK;
}

vmc_status vmc_udp_connect(vmc_udp_transport *u, const char *host, u16 port) {
    if (!u || !host || port == 0) return VMC_ERR_INVALID_ARG;

    vmc_status st = udp_open(&u->base);
    if (st != VMC_OK) return st;

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      /* IPv4 for Phase-1 LAN */
    hints.ai_socktype = SOCK_DGRAM;

    char portstr[8];
    (void)snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    int rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0 || !res) {
        VMC_LOGE("vmc_udp_connect: getaddrinfo(%s): %s", host, gai_strerror(rc));
        if (res) freeaddrinfo(res);
        return VMC_ERR_IO;
    }

    int c = connect(u->fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (c != 0) {
        VMC_LOGE("vmc_udp_connect: connect failed: %s", strerror(errno));
        return VMC_ERR_IO;
    }
    return VMC_OK;
}
