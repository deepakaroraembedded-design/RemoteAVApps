# VMC A/V Agentic Loop — install & run

**Workload: one 21-minute clip, played end to end, in a single stretch.**
Statistics are collected for every 60 seconds of playback, and every minute is
gated on its own.

Drop this tree at the root of the VMC thin-client repo on the **MEC host**
(`192.168.0.126`). That host runs the Claude Code session (the Main Agent) and
the LL-DASH server; the thin client (`192.168.0.145`) is driven over ssh.

```
AGENTIC_AV_PLAN.md                  the plan (read this first)
CLAUDE.md                           Main Agent standing instructions
docs/AV_TELEMETRY_SPEC.md           Phase 0: what to instrument in the C code
.claude/agents/av-test-runner.md    the ONE subagent (measurement only)
.claude/commands/av-loop.md         /av-loop entry point
tools/av_harness/env.sh             topology, paths, tiers, gates  (edit first)
tools/av_harness/remote.sh          ssh wrapper, hard timeout
tools/av_harness/relaunch.sh        clean-slate restart, verified, --play-once
tools/av_harness/collect.sh         precheck + warm-up + window + heartbeat + res sampling
tools/av_harness/av_report.py       NDJSON -> per-minute buckets -> gates -> report
tools/av_harness/selftest_synth.py  proves the gates detect their own faults
tools/av_harness/schema/            report JSON schema
```

## Install

```sh
cp -r vmc-agentic/. /path/to/vmc-thinclient/
cd /path/to/vmc-thinclient
chmod +x tools/av_harness/*.sh tools/av_harness/*.py

# 1. topology, paths, the clip, the gates
$EDITOR tools/av_harness/env.sh        # INPUT_CLIP + CLIP_DURATION_S=1260
#    The 21-min clip is already sourced and frozen at $REPO/4K/swiftrade_av_21min.mp4
#    (sha256 48304bd8…, native 1080p60 + AAC 44.1 kHz, stream-copied from 4K/Earth.mp4).
# 2. passwordless ssh to the thin client
ssh-keygen -t ed25519 -N '' -f ~/.ssh/vmc_agent
ssh-copy-id -i ~/.ssh/vmc_agent.pub deepak7121@192.168.0.145
tools/av_harness/remote.sh 'echo ok'          # must print: ok

# 3. prove the harness can see bugs before trusting it to declare success
python3 tools/av_harness/selftest_synth.py           # 22/22, seconds
python3 tools/av_harness/selftest_synth.py --full    # 22/22 at true 21-min scale
```

## Run

```sh
claude          # from the repo root on 192.168.0.126
> /av-loop 20
```

The Main Agent refuses to start iterating until Phase 0 lands: per-frame
telemetry, `--play-once` on the dash-sim, and EOS handling on both sides.

## The two agents

| | Main Agent | `av-test-runner` |
|---|---|---|
| Runs as | the Claude Code session | a subagent, one spawn per measurement |
| Writes code | yes | **never** |
| Measures | **never** | yes, one run per spawn |
| Output | commits + ledger entries | one `av_report.json` |

The agent that writes the fix is not allowed to decide whether it worked.

## Loop shape (two tiers)

A full iteration costs ~30 minutes, so a 3-minute smoke run pre-filters gross
faults first. Smoke can FAIL fast; only a full 21-minute run can PASS, because
drift and leaks are invisible at 3 minutes (the report skips those gates below
5 buckets rather than firing them on noise).

```
build → ctest → relaunch → smoke (3 min) → relaunch → full (21 min) → triage
```

## What "zero failures and perfect A/V sync" means numerically

**Per minute**, for all 20 complete buckets: A/V offset mean ≤ 10 ms, p95 ≤ 20 ms,
≤ 0.1 % of frames outside the EBU R37 +40/−60 ms envelope; frame yield ≥ 99.5 %
with zero drops and repeats; zero XRUNs, FIFO/jitter/DRM-pool over- and
underflows, fetch failures and 404s, vsync/flip/CUDA-async faults,
`clock_fallback`, and decoder errors.

**Per run**: drift ≤ 0.5 ms/min (Theil–Sen over the 20 per-minute means),
total excursion ≤ 15 ms, p95 degradation ≤ 0.3 ms/bucket, RSS growth ≤ 32 MB,
fd growth ≤ 8, disk bounded and flat over the last 10 minutes, EOS reached
within 5 s of 1260 s, and zero encoder restarts.

Then two consecutive full-run PASSes, then the confirmation matrix: DRM path,
framebuffer path, a second 21-minute clip, and a back-to-back replay (~42 min).

## Self-test coverage

`selftest_synth.py` fabricates telemetry for 21 scenarios and asserts the right
verdict *and* the right `primary_fault`, at both 3-minute and full 21-minute
scale:

```
healthy PASS · av_skew · frame_drops · network_fail · network_slow
audio_underflow · vsync_miss · decode_errors · decoder_unavailable
no_telemetry · fifo_low · present_delay_growth · present_delay_high
--- long-stretch specific, invisible to a 60 s test ---
transient_xrun_min2 · transient_skew_min2 · progressive_drift (0.9 ms/min)
memory_leak (120 MB) · fd_leak (40) · disk_unbounded (900 MB) · no_eos · early_eos
```

Plus a **localisation** check: a transient injected into minute 2 must fail
bucket 2 and only bucket 2. Run it after any change to the report or the gates.

Two design decisions came out of running it at full scale:

- **Drift uses Theil–Sen, not least squares.** A single bad minute is a step,
  and a step near the start tilts an LS fit enough to look like drift — the loop
  would then hunt a servo bug that does not exist. The LS value is still
  reported; disagreement between the two is itself a transient signal.
- **`longrun` is triaged before `av_sync`.** Over 21 minutes a slow drift
  eventually pushes late buckets past the per-minute A/V gate, so when both are
  red the drift is the cause and the bucket failures are the symptom.
