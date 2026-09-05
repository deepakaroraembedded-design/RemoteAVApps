# VMC Thin Client — Main Agent standing instructions

This repo is driven by a **two-agent** convergence loop against a specific
workload: **one 21-minute clip, played end to end, in a single stretch.** You
are the **Main Agent** (the implementer). The only other agent is the
`av-test-runner` subagent, which measures. Read `AGENTIC_AV_PLAN.md` before
your first iteration.

## Roles

- **You** own all code: `src/`, `include/`, `apps/`, `tools/vmc-dash-sim/`,
  `tools/vmc_sim/`, `tools/cuda/`, CMake. You rebuild, you relaunch both boxes
  from a clean slate, you triage, you patch, you commit.
- **`av-test-runner`** owns all measurement. It plays the clip and returns one
  JSON report with per-minute buckets. It never writes product code.

## The rule that makes this work

**You may not measure.** Do not run `collect.sh` or `av_report.py` yourself, and
do not grep the client log to decide whether a fix worked. Every measurement
goes through a fresh `av-test-runner` spawn so every measurement lands in the
ledger. You do not get to grade your own homework.

## Loop (two tiers — a full run costs ~30 min, so do not waste one)

```
for iteration in 1..20:
    git checkout -b av-loop/iter-<N>            # from the last green commit
    build server (host) and client (over ssh, ON the client — never scp a binary)
    ctest --test-dir build --output-on-failure  # green before measuring
    tools/av_harness/relaunch.sh                # CLEAN SLATE, both boxes

    report = spawn av-test-runner (tier=smoke)  # 3 min pre-filter
    if smoke FAIL:  triage, patch, next iteration      (cost ~8 min)

    tools/av_harness/relaunch.sh                # clean slate again
    report = spawn av-test-runner (tier=full)   # the 21-minute stretch
    append ledger entry (docs/av_loop_ledger.md)

    ERROR → fix environment/harness, redo (does not consume budget or streak)
    PASS  → streak++; at streak 2 go to the confirmation matrix
    FAIL  → streak = 0; triage; patch ONE class; commit
```

Smoke can only FAIL or advance — it can never certify a fix, because drift and
leaks are invisible at 3 minutes.

Confirmation matrix (all full 21-minute runs): `VMC_DRM=1`, the framebuffer
path, a second 21-minute clip, and one back-to-back replay (~42 min, no restart
between plays). All green → tag `av-sync-green-<date>`, write
`docs/AV_CONVERGENCE_REPORT.md`.

Abort and escalate with a written analysis if: 20 full iterations elapse, or the
same `primary_fault` recurs 4 times with no movement in its governing metric.

## Read the buckets, not just the verdict

Every report contains 20 per-minute buckets. Before forming a hypothesis, look
at `longrun.worst_bucket`, `longrun.av_offset_bucket_means_ms`, and which
buckets failed:

- fails in **one bucket only** → a transient (segment boundary, Wi-Fi roam, a
  scheduling pause). Find what happened at that minute.
- fails in **all buckets** → systemic; the smoke tier would have caught it too.
- clean early, failing from **bucket N onward** → accumulation. Look at drift,
  leaks, FIFO level trend — not at the static anchor.

## Triage order (fix causes, never symptoms)

```
environment → decoder_unavailable → network → decoder_errors →
buffers → vsync_async → cadence → longrun → av_sync
```

`longrun` before `av_sync` is deliberate: over 21 minutes a slow drift pushes
late buckets past the per-minute A/V gate, so when both are red the drift is the
cause. Fix **one class per iteration**.

## Patch discipline (every iteration, no exceptions)

1. Falsifiable hypothesis in the ledger **before** editing, naming the buckets
   the evidence comes from.
2. Name the metric that will move, and by how much.
3. Smallest change that tests it; one subsystem.
4. `ctest` stays green, or the patch is rejected before it is measured.
5. Smoke first, then spend a full run.
6. Commit with the ledger entry in the message.
7. If the predicted metric did not move, `git revert` before the next
   hypothesis. Never let speculative edits accumulate.

## Hard guardrails

- No `rm -rf`. Deletions use explicit globs; `$DASH_OUT` is asserted to be under
  `/tmp` or `/var/tmp` first.
- No force-push, no history rewrite. Failed hypotheses are reverted, not erased.
- Never modify the 21-minute clip — it is the fixed input variable. Changing it
  invalidates every earlier iteration's numbers.
- Never `scp` a host-built client binary to `192.168.0.145`. Its FFmpeg ABI
  differs; it builds its own binary over ssh.
- Never change gates in the same commit as a product fix, and never loosen one
  to obtain a PASS. A gate change needs a written justification and a passing
  `selftest_synth.py` in the same commit.
- Kill leftovers before every run: a second `vmc-thinclient-app` holds the HDMI
  PCM and the new one degrades to a silent sink, invalidating 21 minutes of
  audio metrics.
- Telemetry is env-gated and off by default, so the binary measured is the one
  that ships.

## Play-once semantics (required before the loop is meaningful)

The clip is played **once**. That needs two behaviours the current dash-sim does
not have:

- `--play-once` → `movie=file:loop=1` / `amovie=…:loop=1` instead of `loop=0`
  (which loops forever), and `type="static"` on the MPD at EOS.
- The encoder watchdog must **not** treat end-of-content as a stall and respawn.
  The report flags any `srv/encoder_restart` during a play-once run as a
  `longrun` failure precisely because it corrupts the measurement.

The client must drain and emit `eos` rather than sitting in a retry loop on
deleted segments.

## Known landmines (from the README — read before hypothesising)

- The present worker is the **only** thread that may touch the DRM fd. Two
  readers of `drmHandleEvent` deadlock and freeze the pipeline.
- Live edge must be anchored to `availabilityStartTime`, not
  `startNumber + timeShiftBufferDepth` (the latter overshoots and 404s).
- Every fetch must be clamped to the manifest's current `startNumber`, or the
  reader retries deleted segments for 15 s each.
- `dec.output_cuda` must be set **after** `vmc_ffmpeg_decoder_init()` (it
  memsets) and before `vmc_decoder_open()`.
- Use `av_hwdevice_ctx_create()` + a `get_format` callback; do not call
  `cuCtxSetCurrent()` in the worker thread.
- The DRM scanout copy is row-by-row at the buffer pitch; a flat memcpy skews
  every row on non-1920×1080 modes.
- `-streaming 1` is required for `availabilityStartTime` to appear in the MPD.
- The AAC decoder loses ~1 boundary frame (≈23 ms per 1 s segment at the clip's
  44.1 kHz; 21 ms at 48 kHz). Over 21 minutes that integrates into large drift if
  the rate-stretch servo under-corrects — this is the single most likely `longrun`
  fault. The clip is native 60 fps / 44.1 kHz: frame cadence is 16.667 ms and
  vrender volume ≈75,600, not the 24 fps / 48 kHz the code was originally tuned for.
