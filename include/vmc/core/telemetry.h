/*
 * telemetry.h — machine-readable A/V telemetry for the agentic test harness.
 *
 * Emits one JSON object per line (NDJSON) into a lock-free ring that a single
 * writer thread drains to a file and/or a UDP sink. Env-gated (off by default,
 * zero cost in production):
 *
 *   VMC_TELEMETRY=file.ndjson      append-only file sink
 *   VMC_TELEMETRY_UDP=host:port    UDP sink (STREAM_TELEMETRY datagrams)
 *
 * Hard constraints from docs/AV_TELEMETRY_SPEC.md:
 *   - vmc_tlm_emit() must never block on I/O and never take a mutex, so it is
 *     safe to call from the DRM present worker and the ALSA audio thread.
 *   - Overflow drops events (counted) and emits a tlm_overflow event; it must
 *     never stall the pipeline.
 *   - Envelope: {"t":"<type>","ts":<CLOCK_MONOTONIC us>,"seq":<n>, ...fields}
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_CORE_TELEMETRY_H
#define VMC_CORE_TELEMETRY_H

#include "vmc/core/compiler.h"
#include "vmc/core/types.h"

VMC_BEGIN_DECLS

/* Start the telemetry writer thread from env config. role is "client" or
 * "server"; host is the machine hostname; build is "debug"/"release"; git is a
 * short commit id. Returns true when at least one sink is enabled. Safe to
 * call more than once (closes any previous instance). */
bool vmc_tlm_init(const char *role, const char *host, const char *build,
                  const char *git);

/* Stop the writer thread and close sinks. Safe on an uninitialised/disabled
 * instance. */
void vmc_tlm_shutdown(void);

/* True when a sink is active (emits are recorded). */
bool vmc_tlm_enabled(void);

/* Emit one event. `type` is the event name; `fmt` and the varargs are printf
 * style and produce the extra JSON fields as `"name":value,...` (no braces).
 * Lock-free and non-blocking: on ring overflow the event is dropped (counted),
 * never stalling the caller. */
void vmc_tlm_emit(const char *type, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

VMC_END_DECLS

#endif /* VMC_CORE_TELEMETRY_H */
