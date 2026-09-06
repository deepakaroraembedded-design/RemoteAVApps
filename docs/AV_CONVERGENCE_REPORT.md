# AV Convergence Report — status of the VMC LL-DASH A/V loop

Date: 2026-09-06 · Branch: `av-loop/iter-2` · Ledger: `docs/av_loop_ledger.md`

## What was implemented (Phase 0, complete)

- **Telemetry core** (`src/core/telemetry.c`): lock-free MPSC ring (16k × 384 B),
  writer thread, append-only NDJSON + UDP sinks, 1 Hz `clock_sync`, `tlm_overflow`.
  Env-gated (`VMC_TELEMETRY` / `VMC_TELEMETRY_UDP`), so the binary that ships is
  the one measured.
- **Event surface**: `vrender`/`arender`/`buf`/`net`/`sync`/`dec`/`eos`/`srv`,
  per the `docs/AV_TELEMETRY_SPEC.md` contract, matching what `av_report.py`
  consumes.
- **Play-once**: `vmc-dash-sim --play-once` (lavfi `loop=1`, `type="static"` at
  EOS, watchdog stands down at EOS), client drains and exits 0, exit 3 on
  `decoder_unavailable`.
- **Harness** at repo root (`tools/av_harness/`): env/remote/relaunch/collect/
  av_report/selftest/schema. selftest 22/22 (fast + full scale). Two-box SSH +
  passwordless sudo wired; clean-slate relaunch verified end-to-end.

## Convergence loop progress (iter-001 → iter-006)

| Metric | iter-001 | iter-005 (DRM) | iter-006 (fb0) |
|---|---|---|---|
| Video cadence | 30 fps (2-vblank lock) | 60 fps, 0.06% at 33ms | 60.0 fps, yield 1.0 |
| Video yield | 0.50 | 0.997 | 1.0 |
| av_offset drift | -1818 ms/min | -211 ms/min | **-3 ms/min** |
| audio underflows | 2050/bucket | ~100/bucket (stable) | ~85/bucket |
| present_delay | unbounded growth | +210 ms/min | **stable 4.66 s** |
| RSS growth | — | 20.8 MB | 2.0 MB |

Key roots fixed, in order:
1. DRM flip-cadence phase-lock (double-poll in `drain_events`; µs-as-ms waits).
2. Server `-g` computed from the 24 fps fallback → 1.2 s segments starving audio.
3. Reader slot pool (128×2 MB always full → demux blocked 17 ms/frame).
4. Reader pacing (sleep-until-boundary + fetch time).
5. Audio worker padding → bounded wait; FIFO sizing.
6. DRM phase-lock again: `drain_events` returning at the first event and no
   serialization → submissions no longer land in the driver's flip window.

## Confirmed hardware bound (DRM path)

The DRM panel refreshes at **59.77 Hz** (measured vblank 16730 µs, 99.76% of
flip intervals) while the clip is 60 fps. The video cannot present faster than
the panel, so the ≤0.5 ms/min drift gate is unsatisfiable on the DRM path with
60 fps content. The framebuffer path (`VMC_DRM=0`, no vblank gating) sustains a
perfect 60.0 fps with -3 ms/min drift, proving the pipeline itself is healthy.

## Remaining work

1. **fb0 cadence smoothness**: `frame_interval_p95_err` 56 ms — the
   deadline-paced fb0 presentation isn't vblank-smooth (needs pacing to the
   frame period).
2. **fb0 constant av_offset** ~ -50 ms (anchor correction).
3. **Audio** underflows ~85/bucket and the FIFO-level gate (just-in-time
   delivery) on both paths.
4. **EOS**: reach the end of content on the fb0 path (the drift-free path
   should reach it).
5. Full 21-minute fb0 confirmation-matrix cell.

## Stop conditions assessment

- Iteration budget (20 full runs): not exhausted (5 full/smoke-tier runs used).
- Plateau rule (same `primary_fault` 4× with no movement): the DRM `av_sync`
  drift was traced to a documented hardware bound, so it is recorded as a
  blocker rather than iterated further, per the plan's guardrails.
