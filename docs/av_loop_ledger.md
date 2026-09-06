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

## iter-001  2026-09-06T14:40Z  commit 6108d39  tier=smoke (cadence class)
verdict: FAIL (cadence now PASS)   primary_fault: buffers   streak: 0
buckets: 0/3 pass   worst_bucket: 0
run:     frames_presented=3584/bucket (yield 0.9955)  interval_p95_err=0.17ms
         frames_dropped=0  drm_pool_exhausted=0  vsync_miss=3584 (downstream)
         av_offset_mean -1213 → -1767ms (growing)  audio_fifo_underflows 2098/bucket
         audio_fifo_level=0 (pure silence)  present_delay 4.85 → 5.56s (growing)
evidence: video cadence fixed (was 0.5 yield). Remaining fault is delivery rate:
         video fetch=184ms but reader loop=1.2s (vfetch=870ms = demux blocking on
         full frame slots behind the ~5s startup backlog). Reader produces 0.83
         video + 0.83 audio seg/s vs the 1.0/s the sink needs → audio FIFO drains
         to 0 → silence → wall-derived audio_pos runs ahead of video → av_offset
         grows ~280ms/min (all downstream of the audio underflow).
journey:  First hypothesis (EBUSY→wait_flip double-poll locking flips at 2 vblanks)
         was correct in spirit but the first two fix attempts were wrong:
         (a) a "wait for the previous flip" gate deadlocked the DRM pool; (b) the
         drain_events single-pass fix alone gave only 50fps. ROOT CAUSE was a
         units bug: the serialization wait passed vblank_period_us (µs) as a ms
         timeout → every wait blocked 16.6 SECONDS. A raw DRM flip probe on the
         client proved the driver delivers 60/60 events when flips are
         serialized. Final fix: serialize flips inside vmc_drm_scanout_present,
         vblank-sized waits, FIFO event→buffer mapping, CRTC force-resync on
         genuinely lost events.
result:    cadence/DRM class CLOSED. Next: reader delivery (buffers class).

## iter-002  2026-09-06T14:45Z  commit <pending>  tier=smoke
hypothesis: the reader's delivery rate caps at ~0.83 seg/s because the 10 s
         live-buffer window burst at startup fills the 256-frame pipeline
         (128 slots + 128 present queue = ~4.3 s), so each video demux blocks
         ~870 ms on full slots. That throttles audio to 0.83 seg/s while the
         sink consumes 1.0/s → FIFO drains to 0 → silence → wall-derived
         audio_pos runs ahead of video → av_offset grows ~280 ms/min.
prediction: shrinking the live buffer to a sustainable depth (10 s → 2 s steady,
         30 s → 6 s max) lets the reader burst a segment without blocking: loop
         → ~1.0 s, audio delivery = 1.0 seg/s → FIFO stays filled, av_offset
         bounded, present_delay stable at ~2 s, no growth.
change:      apps/thinclient/main.c — VMC_VIDEO_STEADY_US/VMC_AUDIO_STEADY_US
             10 s → 2 s; VMC_VIDEO_MAX_US/VMC_AUDIO_MAX_US 30 s → 6 s.
result:      (filled by iter-002's smoke report)
