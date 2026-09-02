#include "vmc/core/error.h"

const char *vmc_strerror(vmc_status status) {
    switch (status) {
        case VMC_OK:                    return "success";
        case VMC_ERR_GENERAL:           return "general error";
        case VMC_ERR_INVALID_ARG:       return "invalid argument";
        case VMC_ERR_NOMEM:             return "out of memory";
        case VMC_ERR_NOSYS:             return "not implemented on this platform";
        case VMC_ERR_TIMEOUT:           return "operation timed out";
        case VMC_ERR_BUSY:              return "resource busy";
        case VMC_ERR_IO:                return "I/O error";
        case VMC_ERR_PROTO:             return "protocol violation";
        case VMC_ERR_CORRUPT:           return "corrupt framing / checksum";
        case VMC_ERR_AGAIN:             return "would block, retry";
        case VMC_ERR_OVERRUN:           return "queue overflow";
        case VMC_ERR_NOT_FOUND:         return "not found";
        case VMC_ERR_INVALID_STATE:     return "invalid state for operation";
        case VMC_ERR_NOT_CONNECTED:     return "not connected";
        case VMC_ERR_OUT_OF_SYNC:       return "sequence out of sync";
        case VMC_ERR_NO_MEDIA:          return "no playable media frame";
        default:
            if (status > 0) {
                return "positive application-defined status";
            }
            return "unknown error";
    }
}
