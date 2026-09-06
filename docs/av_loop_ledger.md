# VMC A/V Convergence Loop — Iteration Ledger

Workload: one 21-minute clip (`4K/swiftrade_av_21min.mp4`, 1920x1080@60fps H.264
+ AAC 44.1kHz, 1260 s) played end to end in a single stretch, play-once.

Rules: falsifiable hypothesis before editing; name the metric that will move and
by how much; one subsystem per iteration; ctest green before measuring; smoke
before full; commit with the ledger entry; revert if the predicted metric did
not move.

## iter-000  2026-09-06T13:30Z  commit 7b908ce  tier=env
Phase 0 bring-up: telemetry ring + writer (env-gated), vrender/arender/buf/net/
sync/dec/eos events, srv events, `--play-once` (loop=1, static MPD at EOS,
watchdog stand-down), client EOS drain + exit 0, exit 3 on decoder_unavailable.
Harness tree dropped at repo root; pgrep/pkill -f self-match fixed over ssh;
server-live detection switched from startNumber (never advances with this
muxer) to publishTime. Relaunch verified clean-slate end-to-end (server live +
first vrender on the client within 20 s). selftest_synth 22/22 (fast + full).

## iter-001  2026-09-06T14:05Z  commit e4e00b2  tier=smoke
verdict: FAIL   primary_fault: cadence (worst symptom)   streak: 0
buckets: 0/3 pass   worst_bucket: 1
run:     frames_presented=1794/bucket (yield 0.498)  interval_p95_err=16.83ms
         vsync_miss=1794/bucket  audio_fifo_underflows 4431/9184/3715
         av_offset_mean -35.8s → -96.1s (growing)  present_delay mean 39→99s
evidence: vrender submit spacing = 33.4ms exactly (2 vblanks); decode timing
         "async=33.7ms" is actually the DRM-buffer wait (buffers free only at
         30/s); reader loop vfetch doubles 2.4s→38s (exponential live-edge loss).
         Fetch RTT 6ms, fetch total ~200ms, conv_async bench = 6us — network and
         conversion are NOT the fault. The 30fps presentation is.
hypothesis: the present worker presents immediately (deadlines 20s+ in the past
         once the backlog forms), so every flip submission lands while the
         previous flip is still pending (EBUSY). The EBUSY path calls
         wait_flip(20) = drain_events(20), which double-polls the DRM fd (a
         second full 20ms timeout after handling events), locking submission
         spacing at ~33ms → 30fps flips. The 5-buffer pool then frees at 30/s →
         decode 30fps → reader backs up exponentially → audio FIFO underflows
         and A/V offset grows without bound.
prediction: fixing the present path (never submit while a flip is pending:
         wait for the completion first; make drain_events single-pass with a
         shared deadline, no 20ms tail) restores one flip per vblank → frame_yield
         ≥0.995 and interval_err ≤1.7ms, which unblocks the DRM pool → decode 60fps
         → reader catches up → audio_fifo_underflows→0, av_offset bounded
         (|mean|≤10ms), present_delay growth→≤10ms/min.
## iter-001b  2026-09-06T14:24Z  commit <pending>  tier=smoke
First fix attempt deadlocked: adding a "wait for the previous flip to complete"
gate in the present worker made the LAST submitted flip's completion event never
arrive (present worker stuck in wait_flip, decode starved for buffers, reader
blocked on slots — full pipeline freeze, 11 vrenders in 185 s). Reverted the
present-worker gate; kept ONLY the drain_events single-pass fix (removes the
20 ms double-poll tail from the EBUSY path). Liveness restored (~49 fps during
the first 35 s with residual drm_pool warnings).
## iter-001c  2026-09-06T14:39Z  commit <pending>  tier=liveness
ROOT CAUSE FOUND (units bug): the serialization wait passed vblank_period_us
(16666 MICROseconds) as a MILLISECOND timeout to wait_flip, so every
"wait for the previous flip" blocked for 16.6 SECONDS — which masqueraded as a
deadlock in iter-001b. A raw DRM flip probe on the client proved the driver
delivers every completion event when flips are serialized (60/60). Fix:
serialize flips inside vmc_drm_scanout_present (never submit while one is
pending), wait in vblank-sized ms chunks, force-resync the CRTC only if a flip
is genuinely stuck >1.5 s, and map events to buffers via a FIFO so a lost
event cannot desync the busy/on-screen accounting. Liveness: 59 vrenders/s,
0 force-resyncs, 992 residual drm_pool warnings.
result:      (filled by the next smoke report)



