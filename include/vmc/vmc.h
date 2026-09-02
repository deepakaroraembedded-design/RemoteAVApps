/*
 * vmc.h — umbrella header for the VMC thin client stack.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_VMC_H
#define VMC_VMC_H

#ifndef VMC_VERSION
#define VMC_VERSION "0.1.0-dev"
#endif

/* Common C++ interop guard for every public header. */
#ifdef __cplusplus
#define VMC_BEGIN_DECLS extern "C" {
#define VMC_END_DECLS }
#else
#define VMC_BEGIN_DECLS
#define VMC_END_DECLS
#endif

#include "vmc/core/types.h"
#include "vmc/core/error.h"

#endif /* VMC_VMC_H */
