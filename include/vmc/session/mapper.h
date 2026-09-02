/*
 * mapper.h — mapper/discovery client.
 *
 * The thin client contacts the mapper on boot; the mapper maps the device to
 * its dedicated MEC-hosted Android container and returns a media route.
 * Phase-1 uses a simple UDP request/response (see src/session/mapper.c).
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_SESSION_MAPPER_H
#define VMC_SESSION_MAPPER_H

#include "vmc/core/types.h"
#include "vmc/session/session.h"

VMC_BEGIN_DECLS

typedef struct vmc_mapper_cfg {
    const char *host;      /* mapper address (name or literal) */
    u16         port;      /* mapper UDP port */
    u32         timeout_ms;
    u32         max_retries;
} vmc_mapper_cfg;

typedef struct vmc_mapper_ctx vmc_mapper_ctx;

/* Create the mapper context (heap in the desktop build; override for MCU). */
vmc_mapper_ctx *vmc_mapper_create(const vmc_mapper_cfg *cfg);

void vmc_mapper_destroy(vmc_mapper_ctx *m);

/* Blocking (per-timeout) discovery. Returns VMC_OK and fills cfg with the
 * container route on success. */
vmc_status vmc_mapper_resolve(vmc_mapper_ctx *m, vmc_session_config *out_route);

VMC_END_DECLS

#endif /* VMC_SESSION_MAPPER_H */
