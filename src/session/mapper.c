#include "vmc/session/mapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

#include "vmc/core/error.h"
#include "vmc/core/logger.h"
#include "vmc/core/platform.h"

struct vmc_mapper_ctx {
    vmc_mapper_cfg cfg;
};
vmc_mapper_ctx *vmc_mapper_create(const vmc_mapper_cfg *cfg) {
    if (!cfg || !cfg->host || cfg->port == 0) return NULL;
    vmc_mapper_ctx *m = (vmc_mapper_ctx *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->cfg = *cfg;
    if (m->cfg.timeout_ms == 0) m->cfg.timeout_ms = 1000;
    if (m->cfg.max_retries == 0) m->cfg.max_retries = 3;
    return m;
}

void vmc_mapper_destroy(vmc_mapper_ctx *m) {
    free(m);
}

vmc_status vmc_mapper_resolve(vmc_mapper_ctx *m, vmc_session_config *out_route) {
    if (!m || !out_route) return VMC_ERR_INVALID_ARG;

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    char portstr[8];
    (void)snprintf(portstr, sizeof(portstr), "%u", (unsigned)m->cfg.port);
    if (getaddrinfo(m->cfg.host, portstr, &hints, &res) != 0 || !res) {
        if (res) freeaddrinfo(res);
        return VMC_ERR_IO;
    }

    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return VMC_ERR_IO;
    }
    (void)connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    /* Request: VMC1 <device-id> — Phase-1 plaintext. TLS/DTLS in security
     * iteration. Keep the request tiny (<= 32 bytes). */
    const char req[] = "VMC1 thin-client-01";
    vmc_status result = VMC_ERR_TIMEOUT;

    struct timeval tv;
    tv.tv_sec = m->cfg.timeout_ms / 1000;
    tv.tv_usec = (long)(m->cfg.timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (u32 attempt = 0; attempt <= m->cfg.max_retries && result != VMC_OK; attempt++) {
        ssize_t n = (ssize_t)send(fd, req, sizeof(req) - 1, 0);
        if (n < 0) {
            result = VMC_ERR_IO;
            break;
        }
        char buf[256];
        n = (ssize_t)recv(fd, buf, sizeof(buf) - 1, 0);
        if (n < 0) {
            continue; /* timeout, retry */
        }
        buf[n] = '\0';
        /* Response format: "VMC1 ROUTE <ip>:<port>" */
        char host[VMC_SESSION_MAX_ROUTE];
        unsigned int port = 0;
        const char *route = strstr(buf, "ROUTE ");
        bool parsed = false;
        if (route) {
            route += 6;
            if (sscanf(route, "%127[^:]:%u", host, &port) == 2 &&
                port > 0 && port <= 65535) {
                parsed = true;
            }
        }
        if (parsed) {
            (void)strncpy(out_route->container_host, host,
                          sizeof(out_route->container_host) - 1);
            out_route->container_host[sizeof(out_route->container_host) - 1] = '\0';
            out_route->container_port = (u16)port;
            result = VMC_OK;
        } else {
            VMC_LOGW("mapper: malformed response: %s", buf);
            result = VMC_ERR_PROTO;
        }
    }

    (void)close(fd);
    return result;
}
