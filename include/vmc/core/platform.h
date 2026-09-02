/*
 * platform.h — thin platform abstraction layer.
 *
 * Time: monotonic microseconds. The implementation lives in
 * src/platform/linux/time.c for Linux/glibc and can be swapped for a
 * bare-metal RTC/systick implementation.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_CORE_PLATFORM_H
#define VMC_CORE_PLATFORM_H

#include "vmc/core/types.h"

VMC_BEGIN_DECLS

/* Monotonic time, microseconds. Never goes backwards. */
vmc_time_us vmc_time_now_us(void);

/* Monotonic time in milliseconds. */
u64 vmc_time_now_ms(void);

/* Busy/thread sleep in milliseconds (0 on bare-metal = yield to scheduler). */
void vmc_sleep_ms(u32 ms);

VMC_END_DECLS

#endif /* VMC_CORE_PLATFORM_H */
