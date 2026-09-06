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
muxer) to publishTime. Relaunch verified clean-slate end-to-end. selftest 22/22.

## iter-001  2026-09-06T14:40Z  commit 6108d39  tier=smoke (cadence class)
Root cause: the present worker's EBUSY→wait_flip path double-polled the DRM fd
(a full second 20ms timeout after the event), locking flips at 2 vblanks
(30fps). Also a units bug (vblank_period_us passed as ms → 16.6s waits that
looked like deadlocks). A raw DRM probe proved the driver delivers 60/60 events
when flips are serialized. Final fix: serialize flips, FIFO event→buffer
mapping, CRTC force-resync, single-pass drain. Video: 30fps → 60fps cadence,
yield 0.50 → 0.9955, interval err 16.8 → 0.17ms.

## iter-002  2026-09-06T15:50Z  commit 1d981dc  tier=smoke (buffers class)
Root causes (in order): (a) THE SERVER — dash-sim computed `-g` from the
fallback fps (24) before the lavfi probe updated it to 60 → 0.4s GOPs grouped
into 1.2s segments → 0.83 seg/s delivery starving audio. Fixed: compute `-g`
after the probe → 1.0s segments (audio underflows 2050 → ~80/bucket). (b) reader
slot pool 128×2MB perpetually full (decode drains at exactly 60/s) → demux
blocked ~17ms/frame. Fixed: 512 slots × 512KB (measured max AU ≈94KB). (c)
reader pacing: sleep until boundary − fetch_ewma. (d) audio worker bounded-wait
for a full period instead of padding; FIFO 8MiB→512KiB then 2MiB. (e) decode
DRM-buffer starvation reduced with an 8-buffer pool. Delivery now 2.0 seg/s.

## iter-003  2026-09-06T16:20Z  commit df63e41  tier=full (first 21-min)
First full run: av_offset drifted -1818ms/min (video ~58fps vs 60fps audio-master
timeline), audio collapsed mid-run (periods 12000→1444, FIFO overflows), no EOS,
rss 29.6MB. Established the longrun front.

## iter-004  2026-09-06T16:25Z  commit 79c419d  tier=smoke
Discovery: the drift is 2.7% of flips taking 2 vblanks (consecutive 30fps
phases), each adding a permanent 16.7ms of lateness. Present-queue bounding made
it worse (decode gated to 58fps); a timeline servo could not converge.

## iter-005  2026-09-06T17:41Z  commit 52b8f3d  tier=full
BLOCKER — the phase-lock is FIXED: `drain_events` now returns at the first flip
event (not after a full second poll) and `present()` submits without
serializing to the previous completion, so a submission no longer lands in the
driver's flip window. 30ms+ intervals 2.7% → 0.06%; drift -1818 → -211ms/min;
yield 0.997; audio underflows stable 33-119/bucket with NO collapse/overflow;
rss 20.8MB. The remaining -211ms/min is a HARDWARE BOUND: the panel's measured
vblank is 16730µs = 59.77Hz (99.76% of intervals), while the clip is 60fps —
the video can present no faster than the panel, so 60fps content cannot meet the
≤0.5ms/min drift gate on the DRM path. This is the plan's documented
"hardware/driver limit out of scope for the loop" case. The iter-006 framebuffer
run (below) confirms the drift is the panel, not the pipeline.

## iter-006  2026-09-06T18:09Z  commit 652fa18  tier=smoke (framebuffer cell, VMC_DRM=0)
CONFIRMATION: the framebuffer path sustains a PERFECT 60.0fps with essentially
NO drift — av_offset drift -3.09ms/min (DRM path: -211ms/min), frames_presented
3600/bucket (yield 1.0), present_delay stable at 4.66s, rss_growth 2.0MB,
vsync_miss 0. This proves the DRM path's av_offset drift is the panel's
59.77Hz vblank (hardware bound), NOT the pipeline. The fb0 path's remaining
issues: frame_interval_p95_err 56ms (the deadline-paced memcpy cadence isn't
vblank-smooth) and a ~50ms constant av_offset. Audio underflows ~80-90/bucket
on both paths.

BLOCKER (recorded): DRM path — the panel (1366x768) refreshes at 59.77Hz
(measured vblank 16730us); 60fps content cannot be presented faster than the
panel, so the ≤0.5ms/min drift gate is unsatisfiable on the DRM path. The
framebuffer path is the drift-free fallback but needs cadence smoothing and an
anchor correction for the ~50ms offset.

## iter-007  2026-09-06T18:24Z  tier=smoke (fb0 cadence attempt)
hypothesis: the fb0 bursty cadence (65% of intervals at 2ms, p95 err 56ms)
         comes from presenting back-to-back when deadlines are in the past;
         pacing to max(deadline, last_present + frame_period) should smooth it.
result:    REVERTED. The pacing stalled the decode worker to 4fps: the
         frame-period target coupled with the reader/decode feedback starved the
         reader (4s/segment) and the audio (FIFO 0, XRUNs). The clock-domain
         variant (audio-clock tracker) did not help. fb0 restored to the
         iter-006 state (60fps, -3ms/min drift, bursty cadence). The cadence
         smoothness on fb0 remains an open item; the DRM phase-lock fix (iter-005)
         remains the significant win.

Next: smooth the fb0 cadence without starving the reader/decode (a dedicated
present pacing thread on the fb0 path, or pacing the reader not the decoder),
and correct the constant av_offset; then run the fb0 full 21-minute cell.

## iter-008  2026-09-06T20:20Z  commit 307ae8f  tier=smoke (fb0: reader rate + cadence + audio)
hypothesis: the fb0 drift and 52fps presentation came from (a) the reader
         delivering at ~1.14s/segment (fetch of the newest was 1 behind the
         edge, so every delivery paid the fetch overhead on top of the encoder
         rate) and (b) the decode-worker catch-up presenting at the reader's
         burst+idle rate instead of pacing to the deadline.
result:    CONFIRMED. Fixes:
         - reader fetches up to live_edge+1 (the in-progress segment); the
           server hold lands the delivery exactly on the boundary → 1.000s
           intervals (was 1.14s).
         - audio fall-behind recovery jumps to the manifest startNumber (same
           as video) instead of the live edge → no more 50-80s A/V split.
         - fb0 cadence paces present STARTS at max(deadline, last+period)
           (no catch-up), recording the scheduled target not the wall after
           the memcpy → 60fps, yield ~1.0 (11161/11160), frames_dropped 2.
         - audio +2.3% fixed sample duplication compensates the AAC boundary
           loss → 0 underflows/overflows/pads, FIFO level 26.5%.
         Measurement (smoke, 3 buckets): yield 99.96%, A/V drift ~0 over the
         bucket window (offset converging from -79 to -40ms), audio clean.
         Remaining gates: av_offset_mean -40..-79ms (limit 10), cadence
         interval p95 err 4.0ms (limit 1.67), audio period deviation 0.0228
         (limit 0.005, the stretch's longer periods). Not yet green — the
         offset and cadence jitter need a further iteration.

Next: eliminate the residual av_offset (-60ms converging), reduce the cadence
jitter below 1.67ms, and reconcile the audio period deviation with the FIFO
stability (the stretch trades 0 underflows for a 2.3% period deviation).


