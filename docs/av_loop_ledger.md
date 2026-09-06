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

## iter-003  2026-09-06T16:20Z  commit df63e41  tier=full (first 21-min)
verdict: ERROR (window overran; sanity 22 buckets)   primary_fault: av_sync   streak: 0
buckets: 0/22 pass   worst_bucket: 0
run:     av_offset -3.0s → -37.5s (drift -1818ms/min)  present_delay 6.9→34.2s
         frames_presented 3492/bucket (yield 0.97)  frame_interval_p95_err 0.34ms
         audio_fifo_underflows 35→263/bucket, OVERFLOWS 545→2615 from bucket 4 on
         audio_periods_written collapses 11909→1444 by bucket 14
         eos_reached FALSE (client never signalled end; window stretched 1235→1360s)
         rss_growth 29.6MB (near 32MB gate)  fd_growth 0  disk 47MB
evidence: the video presents at ~58.2 fps (16.7 ms intervals, 98% clean) while the
         audio-master deadline advances at 60 fps — a ~3 % rate shortfall that
         accumulates ~30 ms/s into unbounded av_offset and present_delay. The
         missing ~1.8 frames/s are vblanks skipped when the present queue is
         empty (decode just slower than the present). Audio collapses mid-run
         (periods_written 12000→1444 = the worker is blocked waiting for a full
         period for most of the time) and the 512 KiB FIFO overflows when the
         reader bursts. The client never reaches the clip end → no EOS.
hypothesis: the decode worker's DRM-buffer cycle is marginally slower than the
         vblank (serialization wait + conv + copy ≈ 17.7 ms vs 16.7 ms), so with
         the 2-entry present queue it can never get ahead and the present worker
         starves ~3 % of vblanks. The audio side is a second front: the bounded
         wait for a full period plus the tiny 512 KiB FIFO make the audio worker
         stall whenever the reader's delivery phase drifts.
prediction: giving the decode a small run-ahead (present queue bound 2→6) and
         enlarging the audio FIFO to 2 MiB (still ~30% of the gate at the
         2 s buffer) should let the video sustain 60 fps (drift → <1 ms/min) and
         the audio stay fed, bringing the run to EOS on time.
change:      (next iteration)
result:      buffers/cadence largely converged in smoke; the longrun front is the
         video rate vs audio-master rate and the mid-run audio stall.

verdict: FAIL   primary_fault: av_sync (buffers improved 12x)   streak: 0
buckets: 0/3 pass   worst_bucket: 0
run:     audio_fifo_underflows 34/103/107 (was 2050)  audio_pad 3.5k/11k (was 485k)
         audio_period_deviation 0.009 (was 3.6 — config synced to 48k/240)
         frames_presented 3585 (yield 0.9944)  frame_interval_p95_err 0.13ms
         av_offset -1.3s → -1.9s (drift ~-260ms/min)  present_delay 5.3→5.8s
         FIFO level 0.01% (gate 15% — just-in-time reader + fresh server can
         never accumulate a buffer)
root causes (in order of discovery):
  (a) THE server: the dash-sim formatted `-g` from the fallback fps (24) before
      the lavfi probe updated it to 60 → 0.4s GOPs grouped into 1.2s segments →
      0.83 seg/s delivery starving the audio. Fixed: compute `-g` after the probe
      → 1.0s segments (audio underflows 2050→~80). This was the true buffers root.
  (b) reader slot pool: 128 slots × 2MB was perpetually full (decode drains at
      exactly 60/s), so the demux blocked ~17ms/frame → reader loop 1.2s. Fixed:
      512 slots × 512KB (measured max AU ≈94KB), same 256MB RSS. vfetch 1.1s→212ms.
  (c) reader pacing: sleeping until the boundary then fetching added ~200ms per
      loop. Fixed: sleep until boundary − fetch_ewma (server holds in-progress
      segments). Loop 1.2s→0.95s.
  (d) audio worker padded silence whenever the FIFO briefly dipped below one
      period at a segment boundary. Fixed: bounded wait for a full period.
      FIFO 8MiB→512KiB so the just-in-time level passes the 15% gate.
  (e) decode starved the 5-buffer DRM pool by running ahead; present queue bound
      128→2 gates the decode to the present rate (drm_busy warnings → 0).
result:    buffers class largely fixed. Remaining: av_offset −1.3s constant + drift
         −260ms/min (video presents at its decode rate, ~59.7fps vs the 60fps
         audio-master timeline — the AAC boundary-frame rate loss the plan flags
         as the most likely longrun fault). A timeline servo that shifted the
         deadlines could not converge (the video is content-limited: it cannot
         present ahead of its decode). NEXT class: longrun/av_sync.

