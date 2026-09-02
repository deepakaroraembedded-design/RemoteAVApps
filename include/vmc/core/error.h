/*
 * error.h — status codes for the VMC thin client stack.
 *
 * Convention: vmc_status is 0 on success (VMC_OK), negative on error.
 * Applications can layer their own positive error space on top.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_CORE_ERROR_H
#define VMC_CORE_ERROR_H

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

#define VMC_OK                        0
#define VMC_ERR_GENERAL              (-1)
#define VMC_ERR_INVALID_ARG          (-2)
#define VMC_ERR_NOMEM                (-3)
#define VMC_ERR_NOSYS                (-4)  /* Not implemented on this platform */
#define VMC_ERR_TIMEOUT              (-5)
#define VMC_ERR_BUSY                 (-6)
#define VMC_ERR_IO                   (-7)
#define VMC_ERR_PROTO                (-8)  /* Wire protocol violation */
#define VMC_ERR_CORRUPT              (-9)  /* Framing/checksum failure */
#define VMC_ERR_AGAIN                (-10) /* Would block; retry later */
#define VMC_ERR_OVERRUN              (-11) /* Ring/queue overflow */
#define VMC_ERR_NOT_FOUND            (-12)
#define VMC_ERR_INVALID_STATE        (-13)
#define VMC_ERR_NOT_CONNECTED        (-14)
#define VMC_ERR_OUT_OF_SYNC          (-15) /* Sequence gap / reordering */
#define VMC_ERR_NO_MEDIA             (-16) /* Decoder has no playable frame */

/* Human-readable description; never returns NULL. */
const char *vmc_strerror(vmc_status status);

VMC_END_DECLS

#endif /* VMC_CORE_ERROR_H */
