---
description: Run the two-agent VMC A/V convergence loop against one 21-minute clip played in a single stretch, until zero failures and perfect A/V sync.
argument-hint: "[max-full-iterations] (default 20)"
allowed-tools: Bash, Read, Edit, Write, Grep, Glob, Task
---

Run the VMC LL-DASH A/V convergence loop as the **Main Agent**.

Workload: **one 21-minute clip, played end to end, in a single stretch.**
Max full iterations: $1 (default 20 if empty).

Read `AGENTIC_AV_PLAN.md` and `CLAUDE.md` first, then follow this exactly.

## Preflight (once)

1. `. tools/av_harness/env.sh` and print `av_env_dump` so the human can see the
   boxes, the clip, the tiers and the gates in play.
2. `tools/av_harness/remote.sh 'echo ok'` — abort with a clear message if the
   client is unreachable.
3. Verify the clip: `ffprobe` it and confirm the duration matches
   `CLIP_DURATION_S` (1260 s). A wrong clip silently invalidates the frame and
   bucket counts.
4. Confirm Phase 0 telemetry exists: `grep -rq vmc_tlm_emit src/ include/`.
   If not, **stop and do Phase 0 first** — implement
   `docs/AV_TELEMETRY_SPEC.md`, including `--play-once` on the dash-sim and EOS
   handling on both sides. Iterating without per-frame render timestamps only
   produces confident guesses, and at 30 minutes a run those are expensive.
5. `python3 tools/av_harness/selftest_synth.py` — expect 22/22. Then run the
   §7.2 on-hardware fault injections once, including the **mid-run** ones, and
   confirm the report pins each fault to the right bucket. A harness that
   cannot localise a fault in time must not be trusted to declare a PASS.

## Each iteration

1. `git checkout -b av-loop/iter-<N>` from the last green commit.
2. Build the server on this host; build the client **on the client, over ssh**
   (never scp a binary — its FFmpeg ABI differs).
3. `ctest --test-dir build --output-on-failure` — green before measuring.
4. `tools/av_harness/relaunch.sh` — clean slate on both boxes, verified.
5. **Smoke tier.** Spawn `av-test-runner` with `tier=smoke`. Give it only: the
   repo path, iteration number, commit sha, tier, and run directory. Do not tell
   it what you changed or what you expect. If it FAILs, triage and patch — you
   have spent ~8 minutes, not 30.
6. **Full tier.** Relaunch clean again, then spawn `av-test-runner` with
   `tier=full` for the 21-minute stretch. Expect to wait; do not poll it.
7. Append a ledger block to `docs/av_loop_ledger.md`: verdict, primary_fault,
   buckets passed/total, worst bucket, the bucket-mean series, run-level drift /
   excursion / leak / EOS figures, hypothesis, prediction, change, and the
   previous iteration's result.
8. Act on the verdict:
   - `ERROR` → fix environment or harness, redo. Does not consume the budget or
     reset the streak.
   - `PASS` → streak++. At streak 2, run the confirmation matrix: `VMC_DRM=1`,
     the framebuffer path, a second 21-minute clip, and a back-to-back replay
     (~42 min, no restart between plays). All green → tag
     `av-sync-green-<date>`, write `docs/AV_CONVERGENCE_REPORT.md`, stop.
   - `FAIL` → streak = 0. Read the per-bucket table *before* hypothesising:
     one failing bucket means a transient, all buckets means systemic, failing
     from bucket N onward means accumulation. Triage in causal order
     (`environment → decoder_unavailable → network → decoder_errors → buffers →
     vsync_async → cadence → longrun → av_sync`), fix **one class**, commit.
9. Never edit gates in the same commit as a product fix. Never loosen a gate to
   get a PASS.

## Stop conditions

- Converged (see the PASS path above).
- Budget exhausted → blocker report naming the surviving fault, the hypotheses
  tried, and what evidence would distinguish them.
- Same `primary_fault` four times with no movement in its governing metric →
  plateau. Stop and escalate with a written analysis, not another blind patch.

Report to the human after every iteration in two lines: verdict + primary fault
+ buckets passed/total + the one metric that moved.
