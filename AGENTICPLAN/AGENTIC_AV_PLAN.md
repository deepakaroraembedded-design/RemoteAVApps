# Agentic AI Development Plan — VMC LL-DASH A/V Convergence Loop

**Workload under test: one 21-minute clip, played end to end, in a single
stretch.** Not a loop, not a 60-second sample — one continuous 1260 s playout,
start to finish, every iteration.

**Repo:** VMC Thin Client (C11) · **Server:** `vmc-dash-sim` @ `192.168.0.126:8080`
· **Client:** `vmc-thinclient-app --dash` @ `192.168.0.145`

**Agents: exactly two.**

| # | Agent | Runs as | Job |
|---|---|---|---|
| 1 | **Main Agent** (`av-implementer`) | the top-level Claude Code session | Owns the code. Rebuilds, clean-slate relaunches server + client, reads the test report, triages, patches, commits, loops. |
| 2 | **Automation Test Agent** (`av-test-runner`) | a Claude Code **subagent**, spawned once per measurement | Owns measurement only. Plays the clip, collects per-minute statistics for all 21 minutes, returns a single JSON report. Writes **no** product code. |

They alternate until the test agent returns `verdict: "PASS"` on **two
consecutive full 21-minute runs**, then the confirmation matrix in Phase 3.

---

## 0. What the 21-minute stretch changes

A 60-second sample and a 21-minute stretch fail in different ways, and the
design has to follow the workload:

| | 60 s sample | 21 min single stretch |
|---|---|---|
| Dominant fault | static A/V offset | **accumulating drift** and **transients** |
| Aggregate stats | adequate | **dangerous** — a 20 s glitch at minute 14 vanishes in a 21-minute mean |
| Leaks (RSS, fds, disk) | invisible | measurable and decisive |
| Cost per iteration | ~2 min | ~30 min — a blind retry is expensive |
| End of content | never reached | **reached** — EOS must be handled, not treated as a stall |

Four structural consequences, each of which is implemented in the harness:

1. **Per-minute bucket gating.** Statistics are computed for every 60-second
   bucket and **every bucket must pass on its own**. The run aggregate is
   reported but is never what decides PASS. Finding the minute a fault happened
   in is most of the debugging.
2. **A two-tier loop.** A 3-minute smoke run pre-filters gross faults before
   any 21-minute run is spent. Only a full run can PASS.
3. **Run-level drift, leak and completion gates** that a short run cannot
   express — measured with a *robust* estimator so one bad minute is not
   mistaken for drift (§4.3).
4. **Play-once semantics.** The encoder must not loop, and the watchdog must
   not mistake end-of-content for a stall (§2, Phase 0.8).

---

## 1. Why two agents

The failure mode of a single self-testing agent is that it grades its own
homework: it patches, sees a log line it likes, and declares victory. At 30
minutes per measurement that mistake is expensive, so the split matters more
here, not less. The test agent has no access to the diff, no knowledge of what
was "supposed" to be fixed, and a fixed schema it must fill honestly.

Two hard rules enforce it:

- The test agent's tool allow-list **excludes `Edit`/`Write` on `src/`,
  `include/`, `apps/`, `tools/vmc-dash-sim/`**. It runs the harness and reads
  logs; that is all.
- The Main Agent **must not** invoke the harness itself to "check" a fix. Every
  measurement goes through the subagent, so every measurement is in the ledger.

---

## 2. Topology and access

```
        ┌──────────────────────────────────────────────────────────────┐
        │  MEC host  swiftrade  192.168.0.126                          │
        │  ── Claude Code session runs HERE (Main Agent)               │
        │  ── vmc-dash-sim :8080  NVENC + AAC → LL-DASH CMAF           │
        │     --play-once, 21-minute clip, no loop                     │
        │  ── /tmp/dash_server.log, /tmp/vmc_tlm_server.ndjson         │
        └───────────────┬──────────────────────────────────────────────┘
                        │ HTTP 8080  (Wi-Fi media path)
        ┌───────────────▼──────────────────────────────────────────────┐
        │  Thin client  ai2  192.168.0.145  (wlp6s0)                   │
        │  ── vmc-thinclient-app --dash  (CUVID → DRM/HDMI + ALSA)     │
        │  ── /tmp/dash_client.log, /tmp/vmc_tlm_client.ndjson (~60 MB)│
        └──────────────────────────────────────────────────────────────┘
```

Set up once by the Main Agent:

```sh
ssh-keygen -t ed25519 -N '' -f ~/.ssh/vmc_agent
ssh-copy-id -i ~/.ssh/vmc_agent.pub deepak7121@192.168.0.145
echo 'deepak7121 ALL=(ALL) NOPASSWD: /usr/bin/pkill, /bin/cat /proc/asound/*, /usr/bin/systemctl' \
  | sudo tee /etc/sudoers.d/vmc-agent
```

All client access goes through `tools/av_harness/remote.sh` (ssh, hard 120 s
timeout). No agent ever holds an interactive session on the client.

---

## 3. Phased plan

### Phase 0 — Make the system measurable (Main Agent, one time, ~1–1.5 days)

Full schema: **`docs/AV_TELEMETRY_SPEC.md`**.

| Step | Deliverable | Done when |
|---|---|---|
| 0.1 | SSH + sudo setup above | `remote.sh 'echo ok'` prints `ok` |
| 0.2 | `src/core/telemetry.c` — lock-free SPSC ring, writer thread, file/UDP sinks | `tests/test_telemetry.c` passes; `ctest` green |
| 0.3 | `vrender` from `drm_scanout.c` flip-complete handler | a 21-min run yields ≈75,600 `vrender` lines at 60 fps (≈30,240 at 24 fps) |
| 0.4 | `arender` from `alsa_sink.c`, `snd_pcm_delay`-corrected `render_us` | ≈54,000 `arender` lines at 44.1 kHz (≈59,000 at 48 kHz) |
| 0.5 | `buf`, `net`, `sync`, `dec` events | each forced fault (§7) raises exactly its own event |
| 0.6 | `srv` events from `vmc-dash-sim` | server NDJSON present after a run |
| 0.7 | `eos` event on both sides at end of content | emitted once, at ≈t+1260 s |
| **0.8** | **`--play-once` on `vmc-dash-sim`** | encoder uses `movie=…:loop=1` (play once) instead of `loop=0` (loop forever); at EOS it emits `srv/eos` and marks the MPD `type="static"`; **the watchdog does not respawn the encoder at EOS** |
| 0.9 | Client EOS handling | client drains the FIFO, emits `eos`, exits 0 — it does not sit retrying deleted segments |
| 0.10 | Overhead check | `decode avg` within 1 % of a run with telemetry off |
| 0.11 | Telemetry volume | ~60 MB per 21-min run (60 fps); the file is truncated per run, and the client has ≥1 GB free (prechecked) |

**Design constraint from the README:** the present worker is the *only* thread
allowed to touch the DRM fd. Telemetry emission inside it must be a non-blocking
ring push — never a `write(2)`, never a mutex.

**0.8 is not optional.** The current dash-sim loops the file seamlessly by
design (`movie=file:loop=0`) and its watchdog restarts a stalled encoder. Both
behaviours are wrong for a play-once test: the first never ends, the second
turns a legitimate EOS into a restart in the middle of the measurement. The
report flags any `srv/encoder_restart` during a play-once run as a `longrun`
failure for exactly this reason.

### Phase 1 — Build the harness (ships in this bundle)

```
tools/av_harness/
  env.sh              topology, paths, tiers, gates
  remote.sh           ssh wrapper, hard timeout
  relaunch.sh         CLEAN SLATE: kill → verify dead → purge → start → verify up
  collect.sh          precheck, warm-up, measured window, heartbeat, resource sampling
  av_report.py        NDJSON → per-minute buckets → gates → av_report.json
  selftest_synth.py   proves the gates detect their own injected faults
  schema/av_report.schema.json
```

`av_report.py` is the contract boundary. Its output is the only thing that
crosses from the test agent to the Main Agent.

### Phase 2 — The convergence loop

```
   ┌─────────────────────────────────────────────────────────────────┐
   │  MAIN AGENT                                                     │
   │  1. git checkout -b av-loop/iter-<N>                            │
   │  2. build server (host) + client (over ssh, ON the client)      │
   │  3. ctest --output-on-failure       ← green or the patch is out │
   │  4. relaunch.sh                     ← CLEAN SLATE               │
   │  5. spawn subagent  (tier=smoke, 3 min)  ───────────┐           │
   └─────────────────────────────────────────────────────┼───────────┘
                                                         ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │  TEST AGENT: 3-minute smoke run → report                        │
   └─────────────────────────────────────────────────────┬───────────┘
                                                         ▼
        smoke FAIL ──► triage & patch (cost: ~8 min)     │
        smoke PASS ──► relaunch.sh, spawn subagent (tier=full, 21 min)
                                                         ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │  TEST AGENT: 21-minute single-stretch run                       │
   │    warm-up 15 s → 1235 s measured → 20 complete buckets + tail  │
   │    heartbeat every 30 s (abort a dead run at ~90 s, not 21 min) │
   │    resource sample every 60 s (RSS / fds / disk)                │
   │    → av_report.json: per-bucket verdicts + run-level gates      │
   └─────────────────────────────────────────────────────┬───────────┘
                                                         ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │  MAIN AGENT                                                     │
   │  verdict PASS → streak++ ; streak == 2 → Phase 3                │
   │  verdict FAIL → triage (§5) → ONE class → patch → commit        │
   │  ledger entry either way, naming the worst bucket               │
   └─────────────────────────────────────────────────────────────────┘
```

**Why the smoke tier exists.** A full iteration costs ~30 minutes of wall clock
(build ~3, relaunch ~1, run 21, analysis ~1, patch). A gross fault — a build
that does not decode, a dead ALSA path, a 404 storm — shows up in 3 minutes.
Spending 21 minutes to learn it is waste, and waste is what stops a loop from
converging inside a working day. Smoke can only FAIL or advance; it can never
PASS the iteration, because drift and leaks are invisible at 3 minutes (the
report skips those gates entirely below 5 buckets rather than firing them on
noise).

**Iteration budget: 20 full iterations** (~10 h of wall clock). Stop earlier if
the same `primary_fault` recurs 4 times with no movement in its governing
metric — that is a plateau, and it needs a written analysis, not another patch.

### Phase 3 — Confirmation matrix

Entered after **two consecutive full-run PASSes**. Every cell is a full
21-minute single-stretch run:

| Run | Purpose |
|---|---|
| `VMC_DRM=1` (default) | the shipping path |
| framebuffer path (`VMC_DRM=0`) | proves the fix is not DRM-specific |
| second 21-min clip (different fps/GOP/bitrate) | proves it is not clip-specific |
| back-to-back replay: the clip twice, ~42 min, no restart between | proves nothing accumulates across content boundaries |

All green → tag `av-sync-green-<date>`, write `docs/AV_CONVERGENCE_REPORT.md`
from the ledger, stop.

---

## 4. Metrics and gates

Measured over 1235 s: the 21-minute clip minus a 15 s warm-up (startup burst,
FIFO prefill, the expected first `resync`) and a 10 s tail (EOS drain, whose
underflows are expected and must not count as failures). That yields **20
complete 60 s buckets** plus a short remainder.

### 4.1 Two levels of gate

- **Per bucket** — applied to each of the 20 complete buckets independently.
  One bad minute fails the run.
- **Run level** — drift, leaks, completion. Only meaningful over the stretch.

The trailing remainder bucket is reported but gated only on zero-tolerance
counters; rate statistics over a partial minute are noise.

### 4.2 Per-bucket gates

| Class | Metric | Gate |
|---|---|---|
| av_sync | `av_offset_mean_ms` | \|mean\| ≤ 10 |
| av_sync | `av_offset_p95_ms` | ≤ 20 |
| av_sync | `av_envelope_violation_pct` | ≤ 0.1 % of frames outside **+40 / −60 ms** (EBU R37 / ITU-R BT.1359) |
| cadence | `frame_yield` | ≥ 0.995 (60 fps ⇒ ≥ 3591 of 3600) |
| cadence | `frame_interval_p95_err_ms` | ≤ 10 % of the content frame period (16.7 ms @60 fps ⇒ ≤1.7 ms; 41.7 ms @24 fps ⇒ ≤4.2 ms) |
| cadence | `frames_dropped`, `frames_repeated` | 0 |
| cadence | `audio_period_deviation` | ≤ 0.5 % |
| buffers | fifo/jitter under- & overflows, `drm_pool_exhausted`, `seg_queue_starved`, `audio_pad_samples` | 0 |
| buffers | `audio_fifo_level_min_pct` | ≥ 15 % (early warning before an XRUN) |
| network | `seg_fetch_fail`, `seg_fetch_miss`, `mpd_reload_fail` | 0 |
| network | `seg_fetch_p95_ms` / `max` | ≤ 800 / ≤ 2000 |
| network | `tcp_rtt_p95_ms` | ≤ 30 |
| vsync_async | all nine counters incl. `clock_fallback` | 0 |
| decoder_errors | `decode_errors`, `decoder_reinit`, `corrupt_frames`, `missing_ref` | 0 |
| decoder_errors | `decode_p95_ms` | ≤ 15 |

> **Why the envelope is a rate, not a max.** Over 30,240 frames a single
> outlier is noise; a real glitch puts hundreds of frames outside the envelope.
> Gating on absolute max would fail a good run on one frame and make the loop
> chase ghosts. 0.1 % of a 60 s bucket is ~1.4 frames.

### 4.3 Run-level gates

| Class | Metric | Gate | Why it needs 21 minutes |
|---|---|---|---|
| longrun | `av_offset_drift_ms_per_min` | \|drift\| ≤ **0.5** | 0.5 × 21 ≈ 10 ms total. The README's AAC boundary-frame loss is ~1 frame (≈23 ms per 1 s segment at 44.1 kHz; 21 ms at 48 kHz); if the rate-stretch servo under- or over-corrects, this is where it shows |
| longrun | `av_offset_total_excursion_ms` | ≤ 15 | median of the last 3 minutes vs the first 3 |
| longrun | `av_p95_degrade_ms_per_bucket` | ≤ 0.3 | sync quality must not decay minute over minute |
| longrun | `rss_growth_mb` | ≤ 32 | leak in the segment/FIFO/DRM paths |
| longrun | `fd_growth` | ≤ 8 | leaked sockets per HTTP fetch — 2520 fetches per run |
| longrun | `disk_growth_mb` | ≤ 512, and flat over the last 10 min | proves the rolling window actually deletes |
| longrun | `eos_reached` / `stream_ended_early_s` | must reach EOS, within 5 s of 1260 s | the run has to finish the clip |
| longrun | `srv/encoder_restart` | 0 | in a play-once run, a restart means the watchdog mistook EOS or a stall for a hang |
| network | `resyncs` | ≤ 1 (the startup anchor) | run-level, not per bucket |
| latency | `present_delay_p95_ms` | ≤ 30000 | live-edge lag: wall-clock at vblank minus the content's realtime timestamp (see below). The client's live buffer runs **10 s steady → 30 s max**, so a healthy lag is ~10 s; the p95 ceiling only trips when the reader has permanently lost the live edge while A/V stays locked — a failure `av_sync` cannot see |
| latency | `present_delay_growth_ms_per_min` | ≤ 10 | Theil–Sen over the per-bucket mean lag. Robust to buffer re-aiming steps (by design); catches the client silently accumulating lateness over the stretch |

**Latency is derived, not instrumented.** No new events are needed: the harness
aligns the client monotonic timeline to realtime via `clock_sync`, and maps each
`vrender` frame to its content realtime timestamp as
`content_realtime = avail_start_us + pts_us`, where `avail_start_us` (from
`srv/mpd_update`) is the wall-clock of `availabilityStartTime` (content PTS 0)
and `pts_us` is the frame's container PTS. This is the README's live-edge math:
at wall time T the live content PTS is `(T − avail)`, so
`present_delay = realtime(vblank) − (avail + pts)` is how late the client
presented the frame. A constant server-start offset shifts the mean but not the
growth, which is exactly why the mean is **not gated**. The plan previously
verified A/V *sync* but never the product's headline latency metric.

**Drift is estimated with Theil–Sen (median of pairwise slopes) over the 20
per-minute means, not least squares.** A single bad minute is a *step*, and a
step near the start tilts a least-squares fit enough to look like drift — the
loop would then go hunting for a servo bug that does not exist. The
least-squares value is still reported as `av_offset_drift_ls_ms_per_min`;
disagreement between the two is itself a signal that the run had a transient.

### 4.4 Verdict

```
PASS  = all 20 buckets green AND all run-level gates green
FAIL  = ≥1 product gate red, at any level
ERROR = harness/environment fault (relaunch failed, ssh dead, no telemetry,
        run aborted early, fewer complete buckets than expected)
        — does NOT consume the iteration budget, does NOT reset the streak
```

---

## 5. Triage fault tree

`primary_fault` is the **first** red class in causal order, not the loudest:

```
environment → decoder_unavailable → network → decoder_errors →
buffers → vsync_async → cadence → longrun → av_sync → latency
```

`longrun` sits deliberately **before** `av_sync`. Over 21 minutes a slow drift
or a leak eventually pushes late buckets past the per-minute A/V gate, so a run
with both red is a drift problem showing up as per-minute symptoms. If `av_sync`
came first, the Main Agent would hunt for a static anchor offset when the real
defect is the rate-compensation servo.

What each class means here:

```
1. environment        relaunch/ssh/ALSA/PCM-busy/disk/aborted run. Fix host state.
2. decoder_unavailable h264_cuvid missing, av_hwdevice_ctx_create failed,
                       libnv12conv.so not dlopen-able, CUDA ctx not pushable.
                       README "Why Design B was originally blocked" lists the
                       three known root causes.
3. network            fetch fail/404/latency. Live-edge math, startNumber clamp,
                      server hold-then-404, rolling-window deletion.
4. decoder_errors     corrupt/missing_ref usually mean §3 delivered a hole.
                      Only investigate once §3 is clean.
5. buffers            FIFO underflow, drm_pool exhaustion, seg_queue starve.
6. vsync_async        flip errors, the dual-DRM-fd-reader deadlock the README
                      documents, CUDA stream errors, staging alloc.
7. cadence            drops/repeats/interval jitter with buffers clean.
8. longrun            drift, leaks, EOS, encoder restart. THE class this
                      21-minute workload exists to expose.
9. av_sync            residual static offset. Audio-master anchor, ALSA delay
                      compensation, PTS→deadline map.
10. latency           present-delay growing or beyond the LL-DASH floor while A/V
                      stays locked. Derived from vrender/clock_sync/mpd_update.
                      Note the ~1–3 s floor is a property of LL-DASH itself — only
                      chase it if the delay is growing or far above the floor.
```

**Always read `longrun.worst_bucket` and the per-bucket table before
hypothesising.** "It failed" and "it failed at minute 14 and nowhere else" are
different bugs. A fault confined to one bucket is a transient (a GC-like pause,
a segment boundary, a Wi-Fi roam); a fault spread across all buckets is
systemic; a fault that starts at bucket 9 and never stops is an accumulation.

**Patch discipline, every iteration:**

1. Write a falsifiable hypothesis in the ledger *before* editing, naming the
   bucket(s) the evidence comes from.
2. Name the metric that will move and by how much.
3. Smallest change that tests it. One subsystem.
4. `ctest` stays green or the patch is rejected before it is measured.
5. Smoke run first; only then spend a full run.
6. Commit with the ledger entry in the message.
7. If the predicted metric does not move, `git revert` before the next
   hypothesis. Accumulated speculative edits are how these loops rot.

---

## 6. Clean-slate relaunch

Every measurement starts from a guaranteed-identical state. The README warns
that a leftover client holds the HDMI PCM and the next instance "silently
degrades to a silent sink" — over 21 minutes that would poison the entire run,
so the relaunch **verifies** death rather than assuming it.

```
CLIENT (192.168.0.145)                    SERVER (192.168.0.126)
1. pkill -9 -f vmc-thinclient-app         1. pkill -9 -f vmc-dash-sim
2. wait until pgrep empty (≤10 s)         2. pkill -9 -f 'ffmpeg.*dash'
   else FAIL_ENV                          3. wait until pgrep empty (≤10 s)
3. verify the HDMI PCM is released        4. verify :8080 unbound
   (status != RUNNING) else FAIL_ENV      5. purge $DASH_OUT/*.m4s *.mpd
4. truncate client log + NDJSON           6. truncate server log + NDJSON
                                          7. ffprobe the clip, warn if != 1260 s
                                          8. start vmc-dash-sim --play-once
                                          9. poll live.mpd until it parses AND
                                             startNumber advances twice (≤30 s)
10. start client (VMC_DRM=1, VMC_AUDIO_DEV=hdmi,
    ALSA_CONFIG_PATH=/etc/vmc-audio.conf, VMC_TELEMETRY=…)
11. poll for the first `vrender` event (≤20 s), aborting early on
    `decoder_unavailable`, else FAIL_ENV
```

`FAIL_ENV` short-circuits the iteration: the report returns
`verdict:"ERROR", primary_fault:"environment"`. **Never patch A/V logic on
evidence from a dirty run** — at 21 minutes a wasted run is half an hour.

### In-run monitoring

Because a doomed run is expensive, `collect.sh` watches it:

- **Heartbeat every 30 s** — process alive and telemetry file growing. Three
  consecutive stalls (~90 s) or a dead process aborts the run with exit 23 and
  `verdict:"ERROR"`, instead of waiting out the remaining 18 minutes.
- **Resource sample every 60 s** — client RSS, open fd count, `$DASH_OUT` size,
  written to `resources.ndjson`. This is what feeds the leak gates.

Sampling is deliberately low-frequency and is done by the harness, not by the
agent: extra ssh traffic during the window skews `decode_p95_ms` and Wi-Fi RTT.

### Disk

A 21-minute run writes ~60 MB of telemetry on the client (60 fps ⇒ ~75,600
`vrender` + ~54,000 `arender`) and, if the rolling
window ever fails to delete, up to a few hundred MB of segments on the host.
The precheck refuses to start unless the host has ≥4 GB free for `$DASH_OUT`
and the client ≥1 GB for telemetry. Running out of disk at minute 17 is an
expensive way to discover a full `/tmp`.

---

## 7. Fault-injection self-test

A test framework that cannot detect a bug is worse than none. Two layers:

**7.1 Offline (`selftest_synth.py`, seconds, no hardware).** Fabricates
telemetry for 21 scenarios and asserts the right verdict *and* the right
`primary_fault`, at both 3-minute and true 21-minute scale:

```
healthy PASS · av_skew · frame_drops · network_fail · network_slow
audio_underflow · vsync_miss · decode_errors · decoder_unavailable
no_telemetry · fifo_low · present_delay_growth · present_delay_high
--- long-stretch specific, invisible to a 60 s test ---
transient_xrun_min2 · transient_skew_min2 · progressive_drift (0.9 ms/min)
memory_leak (120 MB) · fd_leak (40) · disk_unbounded (900 MB) · no_eos · early_eos
```

`present_delay_growth`/`present_delay_high` fabricate a client that falls
behind the live edge while A/V stays locked (delays inject into `vrender.ts` /
`frame_idx` mapping only — the latency metrics are derived, so no product code
is needed to exercise them).

Plus a **localisation** check: a transient injected into minute 2 must fail
bucket 2 **and only bucket 2**. Run it after any change to the report or the
gates — including any gate change the Main Agent proposes.

**7.2 On hardware, once, before the first real iteration.**

| Injected fault | How | Expected report |
|---|---|---|
| Decoder unavailable | `VMC_FORCE_DECODER=nonexistent_codec` | `decoder_unavailable`, ERROR |
| Network loss | `tc qdisc add dev wlp6s0 root netem loss 5%` | `network` |
| Mid-run network hit | add `netem delay 300ms` at t+600 s, remove at t+660 s | `network`, failing **bucket 10 only** |
| Audio underflow | `VMC_AUDIO_FIFO_BYTES=262144` | `buffers` |
| A/V skew | `VMC_DEBUG_AV_SKEW_MS=120` | `av_sync`, mean ≈ 120 |
| A/V drift | `VMC_DEBUG_AV_DRIFT_PPM=200` | `longrun`, drift ≈ 0.72 ms/min |
| Present-delay growth | `VMC_DEBUG_PRESENT_DELAY_MS=1500` (injects +1.5 s onto every vblank mapping) | `latency` |
| VSYNC fault | `VMC_DEBUG_DROP_FLIP_EVENTS=1` | `vsync_async` |
| Leak | `VMC_DEBUG_LEAK_KB_PER_SEG=64` | `longrun`, `rss_growth_mb` red |
| Dirty slate | leave an old client running | `environment`, PCM busy |

The mid-run cases are the important ones — they are what proves per-bucket
gating works. Remove all `tc` qdiscs afterwards
(`tc qdisc del dev wlp6s0 root`). The injection env vars stay behind
`#ifdef VMC_DEBUG` permanently; they are how you re-validate the harness after
any harness change.

---

## 8. Guardrails

- **No `rm -rf`.** The purge uses explicit globs, and `$DASH_OUT` is asserted to
  be under `/tmp` or `/var/tmp` before any delete.
- **No force-push, no history rewrite.** Failed hypotheses are `git revert`ed,
  so the ledger and the history agree.
- **Never edit the 21-minute clip** — it is the fixed input variable. Changing
  it invalidates every earlier iteration's numbers.
- **Clip sourcing is frozen.** The clip was sourced **once** from
  `4K/Earth.mp4` (`ffmpeg -ss 0 -t 1260 -i 4K/Earth.mp4 -c copy
  -avoid_negative_ts make_zero`, sha256
  `48304bd8eef908ddf4b28c20e5817b5423dd962eb2955f00ea676eeb39932fc3`): native
  1920×1080@60 fps H.264 + AAC 44.1 kHz stereo, **stream-copied, not re-encoded**
  (a re-encode changes GOP/timestamps and therefore the cadence gates). This is
  the one and only trim of the source; re-sourcing with a different trim point,
  codec, fps or audio rate resets the baseline and the streak.
- **Hardware-bound issues are out of scope for the loop.** The documented
  driver/hardware limits — the DRM dumb-buffer CPU-copy bottleneck, CUDA import
  failures, the DASH ~1–3 s live-edge floor, no DPDK PMD — cannot be fixed by
  patching this codebase. If triage traces a red gate to one of them (e.g. a
  `latency` gate hitting the DASH floor, `decoder_errors` driven by the CPU-copy
  bottleneck), write a **blocker report** naming it instead of iterating: the
  loop must not burn a 10 h budget on a defect with no fix in this repo. The
  `latency` triage note (§5) already says to chase growth, not the static floor.
- **Timeouts everywhere**: ssh 120 s, relaunch 90 s, collect 1500 s, whole
  iteration 45 min. A hung iteration returns ERROR, not silence.
- **The client build stays on the client.** Its FFmpeg ABI differs from the
  host's; never `scp` a host-built binary across.
- **Telemetry off by default** in the shipped config, so the binary that is
  measured is the binary that ships.
- **Harness and gates may not change in the same commit as a product fix**, and
  a gate is never loosened to obtain a PASS. Any gate change requires a written
  justification and a passing `selftest_synth.py` in the same commit.

---

## 9. Iteration ledger

`docs/av_loop_ledger.md`, one block per iteration:

```markdown
## iter-014  2026-09-05T14:22Z  commit 9f2ac31  tier=full
verdict: FAIL   primary_fault: longrun   streak: 0
buckets: 11/20 pass   worst_bucket: 19
run:     drift=+0.91ms/min (theil-sen)  excursion=+17.4ms  p95_degrade=+0.34ms/bucket
         rss_growth=+4MB  fds=+0  disk=+18MB  eos=yes@1259.8s
buckets 0-8 clean; mean offset climbs monotonically 0.3 → 18.9 ms from bucket 9 on.
hypothesis:  the rate-stretch servo corrects on FIFO level only, so the constant
             AAC boundary-frame loss integrates into linear A/V drift. It is
             invisible for ~9 minutes because the FIFO absorbs it first.
prediction:  feeding measured av_offset into the servo drives drift below
             0.2 ms/min and excursion below 5 ms, without changing frames/xrun.
change:      src/audio/alsa_sink.c — servo error = 0.7*fifo_err + 0.3*av_err
result:      (filled in by iter-015's report)
```

The ledger is what a human reads, and what lets the Main Agent notice a plateau
instead of re-trying a dead hypothesis with cosmetic variations.

---

## 10. Files in this bundle

```
AGENTIC_AV_PLAN.md                    ← this document
docs/AV_TELEMETRY_SPEC.md             ← Phase 0 instrumentation contract
CLAUDE.md                             ← Main Agent standing instructions
.claude/agents/av-test-runner.md      ← the ONE subagent
.claude/commands/av-loop.md           ← /av-loop entry point
tools/av_harness/env.sh
tools/av_harness/remote.sh
tools/av_harness/relaunch.sh
tools/av_harness/collect.sh
tools/av_harness/av_report.py
tools/av_harness/selftest_synth.py
tools/av_harness/schema/av_report.schema.json
```

Drop the tree at the repo root, point `INPUT_CLIP` at the 21-minute file, run
`/av-loop` from `192.168.0.126`, and the loop starts at Phase 0.
