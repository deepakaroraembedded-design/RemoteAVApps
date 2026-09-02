/*
 * time.c — Linux monotonic clock + sleep implementation.
 * SPDX-License-Identifier: MIT
 */
#include "vmc/core/platform.h"

#include <time.h>
#include <unistd.h>

vmc_time_us vmc_time_now_us(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (vmc_time_us)ts.tv_sec * 1000000u + (vmc_time_us)(ts.tv_nsec / 1000);
}

u64 vmc_time_now_ms(void) {
    return vmc_time_now_us() / 1000u;
}

void vmc_sleep_ms(u32 ms) {
    (void)usleep((useconds_t)ms * 1000u);
}
