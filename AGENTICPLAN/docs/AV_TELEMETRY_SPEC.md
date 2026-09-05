# VMC A/V Telemetry Spec (v1)

> **Workload:** one 21-minute clip played end to end in a single stretch. The
> sourced clip (`4K/swiftrade_av_21min.mp4`) is native 1920×1080@60 fps + AAC
> 44.1 kHz, so that is ~75,600 `vrender` and ~54,000 `arender` events per run —
> roughly 130k events and ~60 MB of NDJSON. The ring buffer, the file sink and
> the parser are all sized for that; see §6.

Machine-readable telemetry emitted by the thin client and the LL-DASH server so
the **Automation Test Agent** can compute A/V-sync and health statistics without
regex-scraping human-readable log prose.

This is the **Phase 0 deliverable** of the agentic loop: the Main Agent
implements it once, then never has to guess at metrics again.

---

## 1. Why not just parse `dash dbg:`

The existing `dash dbg:` / `dash stats` lines are 5 s *aggregates*. They cannot
answer the two questions the loop actually needs:

1. *When exactly was video frame N scanned out, and where was the audio DAC at
   that instant?* (→ true per-frame A/V offset distribution, not a mean)
2. *Which subsystem produced the first error in a cascade?* (→ ordered, typed
   event stream with monotonic timestamps)

So we add a parallel **event stream**, keeping the existing text lines intact.

## 2. Transport

Two sinks, both enabled by env var. Default off — zero cost in production.

| Sink | Env | Destination | Use |
|---|---|---|---|
| File | `VMC_TELEMETRY=/tmp/vmc_tlm_client.ndjson` | append-only NDJSON | default for the harness |
| UDP | `VMC_TELEMETRY_UDP=192.168.0.126:9998` | `STREAM_TELEMETRY` datagrams | live streaming to the host |

The wire protocol already reserves a `telemetry` stream type
(`include/vmc/transport/protocol.h`), so the UDP sink reuses the existing 17-byte
header with `stream_type = STREAM_TELEMETRY` and an NDJSON payload.

**Writer requirements (hard):**

- Lock-free SPSC ring buffer per producer thread → one writer thread drains it.
  The present worker and the ALSA thread must **never** block on I/O.
- Ring buffer of 64 Ki events (~8 MiB). On overflow, increment
  `tlm_dropped` and emit one `tlm_overflow` event — never stall the pipeline.
- Timestamps are `CLOCK_MONOTONIC_RAW` microseconds from
  `vmc_time_monotonic_us()`, plus one `clock_sync` event per second carrying the
  `CLOCK_REALTIME` pairing so the harness can align client and server streams.

## 3. Event envelope

One JSON object per line, no nesting beyond one level:

```json
{"t":"vrender","ts":123456789,"seq":41207, ...fields}
```

| Field | Type | Meaning |
|---|---|---|
| `t`  | string | event type (table below) |
| `ts` | int    | `CLOCK_MONOTONIC_RAW` µs at emit |
| `seq`| int    | monotonic per-process event counter (gap ⇒ ring overflow) |

## 4. Event types

### 4.1 `clock_sync` — 1 Hz, from the telemetry writer thread

```json
{"t":"clock_sync","ts":…,"seq":…,"realtime_us":1757… ,"host":"ai2","role":"client","build":"debug","git":"a1b2c3d"}
```

### 4.2 `vrender` — **one per presented video frame**, from the DRM present worker

Emitted immediately after the `DRM_EVENT_FLIP_COMPLETE` handler runs for the
frame, so the vblank timestamp is real, not predicted.

```json
{"t":"vrender","ts":…,"seq":…,
 "frame_idx":34512,          // zero-based content frame index (per-segment indexing)
 "seg":1204,                 // CMAF segment number the frame came from
 "pts_us":1438000000,        // container PTS of the access unit
 "deadline_us":…,            // audio-master wall deadline computed for this frame
 "submit_us":…,              // when drmModePageFlip was called
 "vblank_us":…,              // flip-complete event timestamp (scanout instant)
 "buf_idx":3,
 "audio_pos_us":…,           // audio-master clock position read AT vblank
 "late_us":…,                // vblank_us - deadline_us (signed)
 "decode_us":…,              // decode latency for this frame
 "conv_us":…,                // NV12->BGRA convert + copy latency
 "repeat":0}                 // 1 if the same buffer was re-scanned (no new frame ready)
```

`vblank_us` is the **video rendering timestamp**. `audio_pos_us` sampled at the
same instant is what makes the offset exact rather than inferred.

### 4.3 `arender` — **one per ALSA period written**, from the audio worker

```json
{"t":"arender","ts":…,"seq":…,
 "pcm_frames":1024,          // frames handed to snd_pcm_writei
 "hw_pos_frames":68812800,   // snd_pcm_status_get_htstamp / avail-based position
 "trigger_us":…,             // snd_pcm_status_get_trigger_htstamp (stream start)
 "delay_frames":2304,        // snd_pcm_delay -> frames still in the ring/DAC
 "render_us":…,              // ts + delay_frames/44100 -> when these samples hit the DAC
 "content_pts_us":…,         // container PTS of the first sample in this period
 "fifo_level_bytes":…,
 "adelta_ppm":…}             // live rate-stretch servo output
```

`render_us` is the **audio rendering timestamp**.

### 4.4 `buf` — buffer-pressure events (edge-triggered, not per-frame)

```json
{"t":"buf","ts":…,"seq":…,"which":"audio_fifo","event":"underflow","level":0,"cap":8388608,"count":3}
```

`which` ∈ `audio_fifo`, `jitter_buffer`, `frag_assembler`, `drm_pool`,
`seg_queue`, `conv_stage`.
`event` ∈ `underflow`, `overflow`, `starved`, `exhausted`.

> **Definition used by the gates:** *underflow* = a consumer asked for data and
> got less than it needed (ALSA XRUN, present worker with no frame ready,
> decoder starved). *overflow* = a producer had to drop or overwrite because the
> sink was full (FIFO wrap, jitter buffer eviction, DRM pool exhausted so a
> converted frame was discarded).

### 4.5 `net` — one per HTTP segment fetch

```json
{"t":"net","ts":…,"seq":…,"kind":"video","seg":1204,"url":"/chunk-stream0-01204.m4s",
 "status":200,"ttfb_us":18432,"total_us":96110,"bytes":184320,
 "retries":0,"rtt_us":3900,"stall_us":0}
```

Plus one `{"t":"net","kind":"mpd", ...}` per manifest reload. `rtt_us` comes
from `TCP_INFO` (`tcpi_rtt`) on the fetch socket — no extra probe traffic.

### 4.6 `sync` — VSYNC / async-pipeline faults

```json
{"t":"sync","ts":…,"seq":…,"kind":"vsync_miss","detail":"deadline_exceeded","us":19840,"frame_idx":34512}
```

`kind` values and what raises them:

| `kind` | Raised when |
|---|---|
| `vsync_miss` | `vblank_us − deadline_us > 0.5 × frame_interval` |
| `vsync_dup`  | two consecutive flips returned the same buffer (frame repeat) |
| `flip_error` | `drmModePageFlip` returned `-EBUSY/-EACCES/-EINVAL` |
| `flip_timeout` | no `FLIP_COMPLETE` within 3 × frame_interval |
| `drm_event_starve` | present worker waited on the DRM fd with no pending flip |
| `async_cuda` | any `cudaGetLastError()`/`cuStreamQuery` non-success on the conversion stream |
| `async_copy` | `cudaMemcpyAsync`/`conv_wait_event` failure or timeout |
| `async_stage` | pinned staging buffer unavailable / `cudaHostAlloc` failure |
| `clock_fallback` | audio-master clock unavailable → present worker fell back to wall clock |

### 4.7 `dec` — decoder faults

```json
{"t":"dec","ts":…,"seq":…,"kind":"decode_error","codec":"h264_cuvid","rc":-1094995529,
 "msg":"Invalid data found when processing input","frame_idx":34512,"seg":1204,"fatal":false}
```

`kind` ∈
`decode_error` (avcodec_send_packet / receive_frame < 0),
`decoder_unavailable` (`avcodec_find_decoder_by_name("h264_cuvid")` NULL, or
`av_hwdevice_ctx_create` failed, or `libnv12conv.so` `dlopen` failed),
`decoder_reinit` (decoder was flushed/reopened),
`corrupt_frame` (`AV_FRAME_FLAG_CORRUPT`),
`missing_ref` (CUVID reported missing reference frames).

A `decoder_unavailable` event is always `fatal:true` and must abort the run
immediately with a distinct exit code — the Main Agent treats it as an
environment/build fault, not an A/V-sync fault.

### 4.8 `eos` — end of content (client), emitted once

The clip is played **once**, so end-of-stream is a normal, expected event and
must be distinguishable from a stall. Without it the harness cannot tell "the
run finished" from "the run died at minute 19".

```json
{"t":"eos","ts":…,"seq":…,"reason":"end_of_content","last_frame_idx":30239,"last_seg":1259}
```

`reason` ∈ `end_of_content` (server signalled static MPD / no further segments),
`server_gone` (connection lost), `local_abort`.

The client drains the audio FIFO, presents the remaining frames, emits `eos`,
and exits 0. It must not sit in a retry loop against deleted segments.

### 4.9 `srv` — LL-DASH server side (`vmc-dash-sim`), same envelope

```json
{"t":"srv","ts":…,"seq":…,"kind":"segment_publish","seg":1204,"stream":"video","bytes":184320,"encode_us":38200}
{"t":"srv","ts":…,"seq":…,"kind":"hold_404","seg":1300,"held_us":15000000}
{"t":"srv","ts":…,"seq":…,"kind":"encoder_restart","reason":"watchdog_stall"}
{"t":"srv","ts":…,"seq":…,"kind":"mpd_update","start_number":1180,"avail_start_us":…}
{"t":"srv","ts":…,"seq":…,"kind":"eos","reason":"end_of_input","last_seg":1259}
```

**`encoder_restart` during a play-once run is a failure, not a recovery.** The
report counts it as a `longrun` fault: it means the watchdog mistook
end-of-content (or a legitimate slow patch) for a hang and respawned the
encoder in the middle of the measurement, resetting the timeline under the
client.

## 4b. Play-once requirements (`--play-once`)

The single-stretch workload needs two behaviours the current dash-sim does not
have. Both are Phase 0 work, and nothing measured before they land is
trustworthy.

1. **The encoder must not loop.** Today the lavfi pacing is
   `movie=file:loop=0` — in ffmpeg's `movie`/`amovie` filters `loop=0` means
   *loop forever*, which is right for a live simulator and wrong for a
   play-once test. `--play-once` selects `loop=1` (play the file exactly once)
   and, at EOS, rewrites the manifest as `type="static"` and emits `srv/eos`.
2. **The watchdog must not restart at EOS.** Its stall check currently cannot
   distinguish "no new segments because the input ended" from "no new segments
   because the encoder hung". Under `--play-once` it must stand down once EOS
   has been signalled.

## 4c. Resource sampling (harness-side, no code change)

Leak detection over a long stretch does not need in-process instrumentation:
`collect.sh` samples the client once a minute over ssh and writes
`resources.ndjson` alongside the telemetry.

```json
{"t":"res","elapsed_s":600,"rss_kb":268412,"fds":44,"disk_mb":193.0}
```

`rss_kb` from `/proc/<pid>/status`, `fds` from `/proc/<pid>/fd`, `disk_mb` from
`du -sm $DASH_OUT` on the host. These feed the `rss_growth_mb`, `fd_growth`,
`disk_growth_mb` and disk-flatness gates. Sampling is deliberately
low-frequency — one ssh round trip a minute — so it does not perturb
`decode_p95_ms` or the Wi-Fi RTT it is measuring alongside.

## 5. Implementation checklist (Main Agent, Phase 0)

- [ ] `include/vmc/core/telemetry.h` — `vmc_tlm_emit(type, fmt, ...)`, ring init/drain, env parsing.
- [ ] `src/core/telemetry.c` — SPSC ring, writer thread, file + UDP sinks, `clock_sync` ticker.
- [ ] `apps/thinclient/main.c` — init/shutdown the telemetry writer; emit `clock_sync` identity.
- [ ] `src/video/drm_scanout.c` — `vrender`, `sync/vsync_*`, `sync/flip_*`, `buf/drm_pool`.
- [ ] `src/audio/alsa_sink.c` — `arender`, `buf/audio_fifo`, XRUN → `buf underflow`.
- [ ] `src/video/ffmpeg_decoder.c` — `dec/*`, including the `decoder_unavailable` probe at open.
- [ ] `src/transport/dash_reader_direct.c` — `net/*`, `buf/seg_queue`, resync → `sync/clock_fallback`.
- [ ] `tools/cuda` wrapper + `conv_async` call sites — `sync/async_*`.
- [ ] `tools/vmc-dash-sim/*` — `srv/*`.
- [ ] `apps/thinclient/main.c` — `eos` event, FIFO drain, exit 0 (§4.8).
- [ ] `tools/vmc-dash-sim/*` — `--play-once`, `srv/eos`, static MPD at EOS,
      watchdog stands down after EOS (§4b).
- [ ] CMake: build unconditionally, gate emission on the env var (so the *same*
      binary serves prod and test — the loop must not test a different build
      than it ships).

## 6. Volume and cost at 21 minutes

| | per run |
|---|---|
| `vrender` events | 75,600 (60 fps × 1260 s; ≈30,240 at 24 fps) |
| `arender` events | ~54,000 (44.1 kHz / 1024-frame periods; ~59,000 at 48 kHz) |
| `net` events | ~2,520 (audio + video, 1 s segments) |
| total NDJSON | ~130k lines, ~60 MB |

Consequences the implementation must respect:

- The ring buffer (64 Ki events) drains far faster than it fills at ~105 events/s
  average, but the **burst** at startup and at each segment boundary is what
  sizes it. On overflow, count and continue — never stall the present worker.
- The file sink is truncated per run by `relaunch.sh`; the precheck requires
  ≥1 GB free on the client. Do not append across runs.
- `collect.sh` slices the exact window by byte offset and gzips it in transit —
  60 MB raw over Wi-Fi is otherwise a slow, pointless transfer.
- `av_report.py` streams the file line by line and accumulates per-minute
  buckets. It never holds the whole run in memory, and a full-scale report takes
  about a second.

**Latency metrics are derived, not instrumented.** The report's `latency` gates
use only existing fields: `vrender.ts`/`vblank_us` + `clock_sync`
(monotonic→realtime alignment) + `vrender.pts_us` + `srv/mpd_update.avail_start_us`.
`content_realtime(frame) = avail_start_us + pts_us` (`avail_start_us` is the
wall-clock of `availabilityStartTime`, i.e. content PTS 0; `pts_us` is the frame's
container PTS), so `present_delay = realtime(vblank) − (avail_start + pts_us)`.
No extra events are emitted for them. The dash-sim's `mpd_update` must therefore
carry `avail_start_us` (realtime µs) alongside `start_number`.

**Non-negotiable:** telemetry must add < 1 % to `decode avg` and must not add
any lock to the present worker's flip path. The Main Agent verifies this by
running one iteration with and one without `VMC_TELEMETRY` and comparing
`decode avg` / `on-screen avg` from the existing 5 s stats line.
