/*
 * compiler.h — compiler/ABI helpers and the C++ interop guard.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_CORE_COMPILER_H
#define VMC_CORE_COMPILER_H

#include <stdint.h>

#ifdef __cplusplus
#define VMC_BEGIN_DECLS extern "C" {
#define VMC_END_DECLS }
#else
#define VMC_BEGIN_DECLS
#define VMC_END_DECLS
#endif

/* Likely/unlikely branch hints (no-op on non-GCC/Clang). */
#if defined(__GNUC__) || defined(__clang__)
#define VMC_LIKELY(x)   __builtin_expect(!!(x), 1)
#define VMC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define VMC_LIKELY(x)   (x)
#define VMC_UNLIKELY(x) (x)
#endif

/* Mark parameters intentionally unused. */
#define VMC_UNUSED(x) ((void)(x))

/* Byte-swap helpers for little-endian wire encoding. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define VMC_HOST_LITTLE_ENDIAN 1
#else
#define VMC_HOST_LITTLE_ENDIAN 0
#endif

static inline uint16_t vmc_bswap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t vmc_bswap32(uint32_t x) {
    return ((x & 0xff000000u) >> 24) | ((x & 0x00ff0000u) >> 8) |
           ((x & 0x0000ff00u) << 8) | ((x & 0x000000ffu) << 24);
}

static inline uint64_t vmc_bswap64(uint64_t x) {
    return ((uint64_t)vmc_bswap32((uint32_t)(x & 0xffffffffu)) << 32) |
           (uint64_t)vmc_bswap32((uint32_t)(x >> 32));
}

#endif /* VMC_CORE_COMPILER_H */
