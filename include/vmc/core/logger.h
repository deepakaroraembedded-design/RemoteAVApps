/*
 * logger.h — small, embedded-friendly logging with levels.
 *
 * Default sink prints to stderr. On bare-metal targets link a custom
 * vmc_log_sink() implementation (see src/core/logger.c).
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_CORE_LOGGER_H
#define VMC_CORE_LOGGER_H

#include <stdarg.h>

#include "vmc/core/types.h"
#include "vmc/core/compiler.h"

VMC_BEGIN_DECLS

typedef enum vmc_log_level {
    VMC_LOG_TRACE = 0,
    VMC_LOG_DEBUG = 1,
    VMC_LOG_INFO  = 2,
    VMC_LOG_WARN  = 3,
    VMC_LOG_ERROR = 4,
    VMC_LOG_OFF   = 5,
} vmc_log_level;

/* Minimum level to emit; defaults to VMC_LOG_INFO. */
void vmc_log_set_level(vmc_log_level level);

vmc_log_level vmc_log_get_level(void);

/* Overridable sink; default writes to stderr. */
void vmc_log_sink(vmc_log_level level, const char *fmt, va_list args);

void vmc_log_msg(vmc_log_level level, const char *file, int line,
                 const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#define VMC_LOGT(...) vmc_log_msg(VMC_LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define VMC_LOGD(...) vmc_log_msg(VMC_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define VMC_LOGI(...) vmc_log_msg(VMC_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define VMC_LOGW(...) vmc_log_msg(VMC_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define VMC_LOGE(...) vmc_log_msg(VMC_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

VMC_END_DECLS

#endif /* VMC_CORE_LOGGER_H */
