/*
 * platform.c — generic (non-Linux-fallible) time glue.
 *
 * Linux implementation in src/platform/linux/time.c is selected at build
 * time; this file provides nothing but keeps the layout symmetrical.
 * SPDX-License-Identifier: MIT
 */
#include "vmc/core/platform.h"

#if !defined(__linux__)

#include <stdint.h>

/* Minimal portable fallback: monotonic-ish counter seeded from a hook.
 * Real targets should implement vmc_time_now_us() directly. */
vmc_time_us vmc_time_now_us(void) {
    static u64 counter = 0;
    return (counter += 1000);
}

u64 vmc_time_now_ms(void) {
    return vmc_time_now_us() / 1000;
}

void vmc_sleep_ms(u32 ms) {
    (void)ms;
    /* Busy loop; bare-metal targets yield here instead. */
    volatile u64 until = vmc_time_now_us() + (u64)ms * 1000;
    while (vmc_time_now_us() < until) { }
}

#endif /* !__linux__ */
