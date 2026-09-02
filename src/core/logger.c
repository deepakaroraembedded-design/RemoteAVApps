#include "vmc/core/logger.h"

#include <stdio.h>

static vmc_log_level g_log_level = VMC_LOG_INFO;

void vmc_log_set_level(vmc_log_level level) {
    if ((int)level < (int)VMC_LOG_TRACE || (int)level > (int)VMC_LOG_OFF) {
        return;
    }
    g_log_level = level;
}

vmc_log_level vmc_log_get_level(void) {
    return g_log_level;
}

void vmc_log_sink(vmc_log_level level, const char *fmt, va_list args) {
    (void)level;
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

void vmc_log_msg(vmc_log_level level, const char *file, int line,
                 const char *fmt, ...) {
    static const char *const k_level_names[] = {
        "TRACE", "DEBUG", "INFO", "WARN", "ERROR",
    };

    if (level < VMC_LOG_TRACE || level >= VMC_LOG_OFF ||
        level < g_log_level) {
        return;
    }

    const char *lvl = k_level_names[(size_t)level];

    /* Avoid printf overhead when only level filtering is needed in prod. */
    fprintf(stderr, "[%s] %s:%d: ", lvl, file, line);

    va_list args;
    va_start(args, fmt);
    vmc_log_sink(level, fmt, args);
    va_end(args);
}
