/*
 * types.h — common fixed-width types and helpers.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_CORE_TYPES_H
#define VMC_CORE_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "vmc/core/compiler.h"

VMC_BEGIN_DECLS

/* Fixed-width aliases kept short for embedded code. */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

/* Size in bytes. */
typedef size_t   sz_t;

/* Monotonic time, microseconds since an unspecified boot epoch.
 * Use vmc_time_now_us() (platform/time.h) — never wall clock. */
typedef u64 vmc_time_us;

/* Result of a public API call. */
typedef int vmc_status;

VMC_END_DECLS

#endif /* VMC_CORE_TYPES_H */
