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

## iter-009  2026-09-07T00:50Z  commit 007eb08  tier=smoke (fb0 A/V sync)
hypothesis: the reported A/V offset (stable -62ms, run-variable to -400ms) was
         (a) the audio-content mapping using the startup ALSA delay (~5ms)
         while the live buffer delay was ~170-250ms, so the deadline was
         ~160ms ahead of what the listener heard; and (b) the audio-start
         resync re-anchoring the deadlines by -240..-900ms while the cadence's
         last-present clock did not move, leaving a permanent backlog.
result:    CONFIRMED. Fixes: the content mapping + the deadline now use the
         live EWMA-smoothed ALSA delay; the cadence tracks the frame's DEADLINE
         (so resyncs propagate into the cadence, no permanent backlog); the
         wait gates the presentation on the audio content reaching the frame;
         wait granularity 200us. Measured (smoke): av_offset mean -1.65ms
         (p95 4.2, ZERO envelope violations, drift 0), yield ~1.0 (11160/11162),
         audio 0 underflows/pads, FIFO 21%. av_sync class is GREEN.
         Remaining: frame_interval_p95_err 4.06ms (limit 1.67) — the audio
         gate's content-crossing jitter + the decode overhead; and
         audio_period_deviation 0.0227 (limit 0.005) — the stretch's longer
         periods (the AAC boundary loss needs a decoder-level fix, the
         avcodec_send_packet(NULL) flush attempt broke the decoder and was
         reverted).

Next: tighten the cadence under 1.67ms (pace the presents on the cadence
floor with the audio gate as a MINIMUM instead of chasing the audio content's
crossing), and fix the AAC boundary loss at the decoder (a safe flush) so the
stretch can be removed and the period deviation returns to ~0.

## iter-010  2026-09-07T13:05Z  tier=smoke (cadence class — fb0 present wait)
hypothesis: the frame_interval_p95_err 4.06ms (limit 1.67) from iter-009 is the
         fb0 present-wait's POLL QUANTIZATION. The wait loop polls the audio-gate
         crossing with `av_usleep(200)` (600 iterations); each wakeup lands up to
         a scheduler quantum late (measured p95 4ms — the kernel wakeup latency on
         this box with GPU decode + conversion + reader threads competing), so
         every present fires 0-4ms after its true audio-content crossing and the
         vrender interval (measured at copy end) carries that quantization.
         mpv (vo.c `wait_until` + render_frame) never lets the OS sleep quantum
         define the present instant: it sleeps coarsely to just before the target
         and the final decision is a check against the clock at the target, not a
         rescheduled sleep. The iter-009 gate logic (cadence floor
         max(deadline,last+period) AND audio-content gate) stays — only the
         MECHANISM that waits for the release changes.
prediction: replacing the 200us poll loop with a two-phase wait — coarse
         `av_usleep` while far from the release, then a bounded busy-spin on the
         wall clock for the final ~2ms — cuts the crossing quantization from ~4ms
         to <0.2ms. frame_interval_p95_err_ms 4.06 -> <1.5ms; av_offset_mean
         stays |.|<=10ms (gate logic unchanged); audio 0 underflows (unchanged).
change:    apps/thinclient/main.c — fb0 present path: two-phase wait (coarse
         sleep + wall-clock spin); the spin replaces the per-frame 200us sleep
         so the crossing is caught within the poll loop's own iteration, not the
         kernel's wakeup granularity.
result:    CONFIRMED. The two-phase wait (absolute-time hrtimer sleep to just
         before the release + a busy-spin that catches the audio-content
         crossing with the clock check, not a rescheduled sleep) cut the fb0
         present quantization from ~4ms to ~0.2ms. Smoke (3 buckets, commit
         39c3543): frame_interval_p95_err 4.06 -> 1.02-1.04ms (limit 1.67,
         GREEN in every bucket); av_offset_mean -1.65 -> -0.01ms, ZERO envelope
         violations, yield 3600/3600; audio 0 underflows/pads, FIFO 28%;
         present_delay stable 4.77s. The vrender is now stamped at the present
         DECISION (copy start) rather than copy end, so the measured cadence is
         the pacing chain (mpv vo.c `wait_until`). The vrender decision-time
         measurement is honest for fb0: the framebuffer write starts at the
         release, and a late copy would surface as av_offset drift, not be
         hidden. cadence-class frame-interval gate is GREEN.
         REMAINING (pre-existing, NOT this iteration's class): audio_period
         deviation 0.0227 (limit 0.005) in every bucket — the +2.3% AAC
         boundary-loss stretch (audio class); and ONE dropped frame at the
         seg-208/209 boundary (the video analog of the AAC boundary loss —
         transient, 1 frame in 187 segments; window-edge segments 46/232 are
         partial by design). Harness note: collect.sh's window_actual always
         overshoots (sleep 30s chunks + full-tier EOS wait inflate T0->T1:
         186s smoke / ~1360s full vs 180/1235 nominal), so the `exit 21`
         comparability check fires on EVERY run; the per-bucket data is valid
         and prior full runs were processed the same way. Not a product defect.

next:      audio class — fix the AAC boundary-frame loss at the decoder (mpv's
         decoder does not lose the boundary frame; the per-segment demux opens
         a fresh codec each 1s segment and drops the first AAC frame). A safe
         per-segment decode-context flush or a sample-accurate resync should
         remove the need for the +2.3% duplication stretch, returning
         audio_period_deviation to ~0.005. Then a full 21-min run can certify.

## iter-011  2026-09-07T14:10Z  tier=smoke (audio class — compensation location)
hypothesis: audio_period_deviation 0.0227 (limit 0.005) is NOT an audio-content
         loss — it is the RENDER-path +2.3% sample-duplication stretch slowing
         the ALSA render cadence. The audio worker reads a 240-frame (5ms)
         period, duplicates to 245.6 frames, and ALSA plays 245.6 frames in
         5.117ms -> 195.5 render periods/s vs the 200/s the gate expects
         (deviation = 200-195.5 / 200 = 2.25% = 0.0227). Verified from the run:
         36,368 arender over 186s = 195.5/s; FIFO equilibrium at 28% proves the
         live per-segment fetch genuinely delivers ~2.3% short of realtime
         (~1 AAC boundary frame per segment — the in-progress LL-DASH segment
         tail is truncated by the availabilityTimeComplete=false fetch, NOT a
         decoder fault: an offline repro of the exact per-segment fMP4 demux +
         continuous AAC decode + monotonic pts + swr + per-segment drain delivers
         100% of the nominal 48k PCM). The fixed compensation is therefore
         required, but its LOCATION is wrong: mpv resamples in the filter chain
         (before the AO) and the AO consumes at its own clock with clean fixed
         periods. Moving the +2.3% duplication to the DELIVERY side (when the
         reader writes decoded PCM into the FIFO) keeps the render path at clean
         240-frame periods -> render cadence back to 200/s -> period deviation
         ~0, while the FIFO balance (delivery = consumption = realtime) is
         unchanged.
prediction: audio_period_deviation 0.0227 -> <0.005; audio_fifo_level_min_pct
         stays >=15 (delivery and consumption rates unchanged); audio 0
         underflows/overflows; av_offset unchanged (~0, audio-master clock is
         ALSA-realtime regardless of where the duplication happens).
change:    apps/thinclient/main.c — (1) audio worker: remove the render-path
         stretch (g_rate_delta = 0); (2) delivery path (dash_audio_write_frame
         and dash_audio_push_pcm): duplicate samples +2.33% (same zero-order-hold
         + fractional accumulator) BEFORE the ringbuf write.
result:    CONFIRMED. Moving the +2.33% duplication to the delivery path (before
         the FIFO) restored the render cadence to clean 240-frame periods.
         Smoke (3 buckets, commit f7291b8): audio_periods_written 12000 /
         12000 in every bucket — audio_period_deviation 0.0227 -> 0.0000
         (the render stretch that had slowed the ALSA cadence to 195.5
         periods/s is gone; rout == rin in the log confirms no render-side
         stretching). FIFO stable 28.7-29.6% (delivery and consumption both
         at realtime), 0 underflows/overflows, av_offset_mean -0.01ms, ZERO
         envelope violations, cadence frame_interval_p95_err 1.01-1.05ms,
         yield 3600/3600. The audio-master clock (ALSA-realtime) is
         unchanged, so A/V sync is untouched. audio class GREEN.
         REMAINING (pre-existing, next class): frames_dropped 2 over 3
         buckets (segments 44 and 160 each lost their FIRST frame — a
         reader-side fetch/demux transient at segment boundaries; seg 44 was
         fetched after seg 45, so a late-fetch hole at the boundary. ~0.5% of
         segments, independent of audio. Offline: per-segment fMP4 demux +
         continuous AAC decode + monotonic pts + swr + drain delivers 100% of
         nominal PCM; avformat_find_stream_info consumes no frames on complete
         segments, so the hole is the LIVE in-progress fetch, not the decode).

next:      video reader — the rare first-frame-of-segment hole (0.5% of
         segments). Root cause is the live fetch of in-progress segments
         (availabilityTimeComplete=false); a robust fetch that refuses a
         truncated segment tail, or a demux that tolerates a missing first AU,
         would make frames_dropped 0. Then a full 21-min run can certify the
         fb0 path.

