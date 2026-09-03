# VMC Thin Client

Embedded-oriented C11 implementation of the **Virtual Mobile Computing** thin
client device. A lightweight client renders a GPU-encoded video stream coming
from a MEC-hosted Android container, captures touch/sensor/audio input, and
manages the network session — while all heavy compute stays at the edge.

This is a fresh, from-scratch implementation (it does not reuse the earlier
scrcpy/Pi5 prototype scripts).

## Architecture

```
┌──────────────────────┐      UDP (VMC protocol)      ┌───────────────────────┐
│  MEC SIM (host)      │  ──────────────────────────▶ │  THIN CLIENT          │
│  192.168.0.126       │      mapper :9999            │  192.168.0.145 (WiFi) │
│                      │      media  :6000            │                       │
│  ffmpeg h264_nvenc   │                              │  UDP transport        │
│  (RTX 5080)          │                              │  jitter buffer        │
│  → H.264 access units│                              │  fragment reassembly  │
│  → fragment → send   │                              │  CUVID decode         │
│  (C11 poll loop)     │                              │  → NV12 → BGRA conv   │
└──────────────────────┘                              │  → /dev/fb0 → HDMI    │
                                                      └───────────────────────┘
```

Roles:

- **MEC sim** (`tools/vmc_sim/vmc-mec-sim`) — stands in for the ReDroid/MEC
  container. Encodes real H.264 with **NVENC** (ffmpeg `h264_nvenc` subprocess),
  wraps access units in the VMC protocol (fragmenting large frames), echoes
  keepalives, and ingests input batches. A single-threaded `poll()` loop with
  **no Python GIL** — the previous `tools/mec_sim.py` Python simulator is kept
  for comparison but no longer used.
- **Thin client** (`apps/thinclient/main.c`) — receives, reorders, reassembles,
  decodes with **CUVID**, converts NV12→BGRA (CPU swscale or GPU CUDA kernel),
  and presents to the framebuffer/HDMI. A decode worker thread keeps reception
  independent of decode cost.
- **LL-DASH server** (`tools/vmc-dash-sim/vmc-dash-sim`) — optional
  standards-based transport: ffmpeg NVENC+AAC → Low-Latency DASH (CMAF
  segments, dynamic MPD) served over HTTP. The client's `--dash <mpd-url>` mode
  replaces the UDP transport end-to-end (same CUVID decode + DRM scanout).

Two transport modes coexist in one client binary:
- **UDP (VMC protocol)** — the low-latency path (~54 ms motion-to-photon).
- **LL-DASH over HTTP** — the standards path (~1–3 s live-edge latency), chosen
  with `--dash <mpd-url>`.

## Repo layout

```
include/vmc/
  core/       types, error codes, logger, ring buffer, platform/time
  session/    session lifecycle + state machine, mapper client
  transport/  wire protocol, jitter buffer, UDP transport
  video/      decoder/display interfaces, FFmpeg decoder, fragment assembler,
              fb0 backend, DRM scanout backend
  input/      input abstraction, Linux evdev backend, input batching
  audio/      MEC downlink + local mix pipeline, ALSA sink
src/          implementations (platform/linux holds Linux-specific code)
apps/         thin client main application (decode-worker pipeline + overlay)
tests/        unit tests (8 suites, dependency-free harness)
tools/        vmc_sim/ (C/CUDA MEC sender), mec_sim.py (legacy Python sim),
              vmc-dash-sim/ (LL-DASH live server), wifi_connect.sh,
              cuda/nv12_conv.cu
systemd/      vmc-thinclient.service, vmc-wifi.service, vmc-dash-sim.service
```

## Building

Dependencies: CMake, a C11 compiler. Optional, auto-detected:
- **FFmpeg** dev headers (`libavcodec-dev libswscale-dev`) → H.264 decoder
- **libavformat** + **libswresample** dev → DASH demuxer + AAC→PCM resample
- **libdrm** dev headers → DRM scanout backend
- **CUDA** headers (`cuda.h`) → CUVID CUDA device-context path
- **ALSA** dev headers (`libasound2-dev`) → audio playback sink

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

With sanitizers: `cmake -S . -B build-asan -DVMC_ENABLE_SANITIZERS=ON`.

Build flags: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.

The CUDA conversion library is built separately (needs nvcc):

```sh
cd tools/cuda && nvcc -O3 -shared -Xcompiler -fPIC -o libnv12conv.so nv12_conv.cu -lcuda
# install to /usr/local/lib + ldconfig so the client can dlopen it
```

`vmc-mec-sim` and `vmc-dash-sim` are built as part of the main build (linked
against `libvmc_thinclient.a` / `Threads`).

## Running

**1. Start the MEC sim** (on the machine with the encoder GPU, UDP mode):

```sh
./build/tools/vmc_sim/vmc-mec-sim 192.168.0.126 9999 6000 [width] [height] [drop_rate]
# e.g. 1920 1080, or 3840 2160 for 4K (~24 Mbps, NVENC)
# drop_rate defaults to 0.001 (matches the Python sim); pass 0.0 to disable drops
```

The legacy Python sim is still available as `tools/mec_sim.py` with the same
argument order.

**2. Run the thin client** (on the client device, UDP mode):

```sh
./build/vmc-thinclient-app <mapper-ip> <mapper-port> [log-level]
# e.g. ./build/vmc-thinclient-app 192.168.0.126 9999 1
```

To enable the GPU scanout path (Design B) instead of the default framebuffer
path, set `VMC_DRM=1`:

```sh
VMC_DRM=1 ./build/vmc-thinclient-app 192.168.0.126 9999 1
```

An optional green on-screen overlay in the top-left shows live E2E / DEC / NET
latency; it is **off by default** and enabled with `VMC_HUD=1`. Full stats +
p95 are always logged every 5 s.

**3. As persistent services** (systemd, on the client):

Service files are in `systemd/`. Copy them to `/etc/systemd/system/`, edit the
user, home-directory, and Wi-Fi interface (`wlp6s0`) as needed, then:

```sh
sudo cp systemd/*.service /etc/systemd/system/
sudo systemctl daemon-reload
systemctl enable --now vmc-wifi.service       # auto-join Wi-Fi at boot
systemctl enable --now vmc-thinclient.service  # stream at boot (VMC_DRM=1)
```

## Low-Latency DASH mode

The system can also run over **LL-DASH** (Low-Latency DASH, CMAF chunks over
HTTP) instead of the custom UDP protocol. The same decode + display pipeline is
used; only the transport changes.

**1. Start the LL-DASH server** (on the machine with the encoder GPU):

```sh
./build/tools/vmc-dash-sim/vmc-dash-sim --input <file.mp4> [--port 8080] [--fps N]
# encodes the file with NVENC + AAC and serves a live dynamic MPD with
# SegmentTimeline, availabilityTimeOffset and progressive CMAF segments
```

A systemd unit is provided: `systemd/vmc-dash-sim.service` (run as a user
service on the host: `systemctl --user enable --now vmc-dash-sim`).

**2. Run the client in DASH mode:**

```sh
VMC_DRM=1 ./build/vmc-thinclient-app --dash http://192.168.0.126:8080/live.mpd 1
```

The client uses a **direct segment-fetch reader** by default. It fetches the
manifest and CMAF segments with a non-blocking, `poll()`-based HTTP client that
resets its deadline on every progress event, prepends the init segment, demuxes
in memory, converts AVCC→Annex-B, decodes with CUVID, and plays AAC audio
through the same ALSA sink. A 2-segment live-edge lag keeps the client behind
the server edge so it receives complete segments instead of waiting on
progressive writes. The MPD open retries (30×2 s) and both the server and the
reader run watchdog restarts so the stream self-heals. To use the old
`libavformat` DASH demuxer instead, set `VMC_DASH_LIBAV=1`.

### DASH playback quality

The DASH path is tuned for smooth, glitch-free A/V at the live edge:

- **Live-edge math uses absolute segment numbers.** The reader computes
  `live_edge = 1 + (now − availabilityStartTime)/seg_duration`, anchored to the
  manifest's wall-clock start, instead of `startNumber + elapsed`. This stays
  correct after the dynamic MPD window rolls `startNumber` forward (a stale
  anchor made the client request phantom segment numbers and stall).
- **Presentation is paced to the content frame rate.** The reader bursts each
  1 s segment's frames into a 32-slot pipeline; a decode worker paces the DRM
  page flips to `frameRate` (parsed from the MPD) so playback is a steady
  cadence instead of a burst-then-idle judder.
- **DRM flip bookkeeping is correct.** The flip-complete handler frees the
  buffer that was *actually* submitted (tracked via `flip_pending`), and a
  page-flip that hits a busy CRTC waits for the pending flip and retries —
  eliminating the `PageFlip: Device or resource busy` drops and pacing
  presentation to the display vblank.
- **Audio is delivered one segment ahead** of video into a 512 KiB FIFO
  (pre-filled before playback starts), so the per-segment bursts never underrun
  the ALSA sink.
- **The encoder loops indefinitely.** The lavfi source uses
  `movie=file:loop=0,setpts=N/(fps*TB)` — the separate `loop=loop=0:size=N`
  filter exits at the end of the file on FFmpeg 4.x, forcing a restart every
  file length. The dash-sim watchdog's stall check also guards against unsigned
  underflow (a marginally-future chunk mtime no longer triggers a spurious
  restart that resets the live timeline).

Measured over a sustained run (1080p24 NVENC, WiFi 6E): the reader fetches
exactly 1.0 segment/s with **no skipped segments**, the client decodes/presents
at the full **24 fps**, there are zero flip failures or decoder warnings, and
the audio FIFO holds a healthy 50–200 KB. Live-edge lag is ~1–2 s (the
configured 2-segment safety lag). Under sustained GPU load the client
occasionally drops to ~15 fps once the per-frame decode work exceeds the
41.7 ms frame budget — a device throughput limit, not a pipeline error.

## Deployment guide (live topology)

### Network

| Machine | Hostname | IP / subnet | Interface | Role |
|---|---|---|---|---|
| MEC host | `swiftrade` | `192.168.0.126/24` | `enx80691a1594ec` (Realtek USB r8152, 1 GbE) | MEC sim + LL-DASH server + NVENC encode (RTX 5080) |
| Thin client | `ai2` | `192.168.0.145/24` | `wlp6s0` (MediaTek MT7902, Wi-Fi 6E) | CUVID decode + DRM/HDMI display + ALSA audio |

- Subnet `192.168.0.0/24`, gateway `192.168.0.1`, both hosts DHCP.
- The client also has a wired NIC `enp5s0` = `192.168.0.23` (Realtek r8169) —
  the media path uses **Wi-Fi** (`wlp6s0`), confirmed by RX counters.
- No DNS required — the client talks to `192.168.0.126` by IP.

### Ports

| Port | Host | Service | Used by |
|---|---|---|---|
| 9999/UDP | `192.168.0.126` | mapper (UDP mode) | client discovery |
| 6000/UDP | `192.168.0.126` | media (UDP mode) | client A/V stream |
| 8080/TCP | `192.168.0.126` | LL-DASH HTTP server | client manifest + segments |

### 1. Build (on both machines)

```sh
# dependencies (dev headers): libavcodec/libavutil/libswscale,
# libavformat, libswresample, libdrm, alsa-lib, CUDA
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
# optional GPU conversion library (host):
cd tools/cuda && nvcc -O3 -shared -Xcompiler -fPIC -o libnv12conv.so nv12_conv.cu -lcuda
```

The client device builds its own binary (its FFmpeg is a newer ABI than the
host's — do not copy the host-built binary across).

### 2. MEC host (`192.168.0.126`) — encoder + servers

UDP mode (custom VMC protocol, ~54 ms):

```sh
./build/tools/vmc_sim/vmc-mec-sim 192.168.0.126 9999 6000 1920 1080 0.0
# 4K: 3840 2160; drop_rate 0.0 for a lossless test link
```

LL-DASH mode (standards-based, ~1–3 s):

```sh
./build/tools/vmc-dash-sim/vmc-dash-sim \
  --input /home/deepak7121/FLUX3/1225am0807/swiftrade_av_60s_20260807_110902.mp4 \
  --port 8080
# persistent: systemctl --user enable --now vmc-dash-sim
```

### 3. Thin client (`192.168.0.145`) — display + audio + service

Audio output to the HDMI monitor (one-time setup):

```sh
sudo usermod -aG audio deepak7121
printf 'pcm.hdmi { type hw; card 1; device 3; }\n' | sudo tee /etc/vmc-audio.conf
# the system ALSA config on this device cannot resolve card indices, so the
# service uses this minimal config via ALSA_CONFIG_PATH=/etc/vmc-audio.conf
# and VMC_AUDIO_DEV=hdmi (card1 = NVIDIA HDA, device 3 = connected LG monitor)
```

UDP mode (systemd default):

```sh
sudo cp systemd/vmc-thinclient.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now vmc-thinclient
# ExecStart: vmc-thinclient-app 192.168.0.126 9999 1   (mapper -> media route)
```

LL-DASH mode:

```sh
VMC_DRM=1 VMC_AUDIO_DEV=hdmi ALSA_CONFIG_PATH=/etc/vmc-audio.conf \
  ./build/vmc-thinclient-app --dash http://192.168.0.126:8080/live.mpd 1
```

### 4. Verify

```sh
# host: encoder running + segments being produced
pgrep -a vmc-mec-sim; pgrep -a ffmpeg
curl -s http://192.168.0.126:8080/live.mpd | grep -o 'type="[a-z]*"'

# client: decode + audio health
systemctl status vmc-thinclient
journalctl -u vmc-thinclient -n 50 | grep 'dash stats'
sudo cat /proc/asound/card1/pcm3p/sub0/status   # state: RUNNING
```

On the client, `gaps=0 late=0` (UDP) or `pkts≈pub≈decode` (DASH), `decode`
growing at ~24 fps, and ALSA `state: RUNNING` confirm a healthy stream.

## Wire protocol

Single lightweight framed transport over UDP. Header (17 bytes, little-endian):
magic `0x5643`, version, flags, stream type, reserved, payload length,
32-bit sequence, timestamp (µs), crc8 — then a payload. See
`include/vmc/transport/protocol.h`.

Streams: `control`, `video`, `audio`, `input`, `telemetry`.

**Video fragmentation:** access units > 1400 bytes are split; each datagram's
payload starts with a 4-byte fragment header (`frame_id`, `frag_index` with
bit15 = last). The client reassembles before decoding
(`include/vmc/video/fragment.h`).

## End-to-end latency

Measured per frame: the sim stamps each access unit + keepalive-echo with its
monotonic clock; the client derives the sim↔client clock offset from the
keepalive RTT (one-way ≈ RTT/2) and computes

```
e2e = present_time − frame_send_time − offset
```

CUVID's 1-frame output hold is accounted for by mapping the presented frame's
`pts` back to its real sender timestamp.

Representative results (Wi-Fi 6, 4K stream → 1080p display, Design B):

| Metric | Python sim (`mec_sim.py`) | C sender (`vmc-mec-sim`) | Notes |
|---|---|---|---|
| e2e avg | ~119 ms | **~66 ms** | Python sim included a ~50 ms GIL/stamping bias, now removed |
| e2e min | ~101 ms | **~42 ms** | |
| e2e p95 | ~127 ms | **~95 ms** | |
| on-screen avg | ~86 ms | **~52 ms** | DRM flip timing (motion-to-photon) |
| decode avg | ~7.0 ms | ~7.0 ms | CUVID + GPU NV12→BGRA, unchanged |
| one-way | ~3–8 ms | ~4 ms | WiFi |
| queue | ~60 µs | ~0.5 ms | decode-worker pipeline |

The residual ~52 ms on-screen is dominated by CUVID's 1-frame hold (~33 ms at
30 fps) plus the DRM flip cadence — not the network or the sender.

## Video conversion: the three paths

The decoder outputs **NV12** (CUVID). It must become **BGRA/XRGB** for the
display. Three implementations exist:

### 1. CPU swscale (fallback)
`swscale` converts NV12→BGRA (+ downscale). ~4.7 ms @1080p, ~13 ms @4K (CPU-bound).

### 2. CUDA kernel — Design A (current default, reliable)
`libnv12conv.so` (`nv12_to_bgra`) runs an NV12→BGRA kernel on the GPU:
```
CUVID nv12 (sysmem) → H2D (12.4 MB) → kernel → D2H (8.3 MB) → fb0
```
Isolated kernel is ~2 ms (vs ~13 ms swscale) — **6.7× faster**. In-pipeline the
decode avg drops 13.0 → 11.5 ms, because CUVID's decode+NV12-copy (~9.5 ms)
dominates and is unchanged. Enabled automatically when the `.so` is present.

### 3. GPU scanout — Design B / Option 1 (working, opt-in via `VMC_DRM=1`)
The full-GPU path: CUVID → CUDA frames → stream-overlapped conversion → DRM
page-flip scanout. The systemd service now enables this by default. Components:

- **`drm_scanout.c`** — 3 dumb XRGB buffers, `drmModePageFlip` with
  `DRM_EVENT_FLIP_COMPLETE`, per-buffer flip timestamps → the host CPU gets
  **vsync-accurate "on screen" timing** (motion-to-photon).
- **Stream-overlapped conversion** (`conv_async`/`conv_wait_event`) — a
  dedicated CUDA stream + **pinned staging**; the queue returns in ~0.04 ms and
  the conversion (~1.5 ms) overlaps with the next frame's decode.
- **`get_format` + `av_hwdevice_ctx_create` decoder mode** — tells CUVID to expose
  decoded frames as CUDA device pointers and creates a CUDA context that CUVID can
  push/pop on this driver/FFmpeg combination.

**Why Design B was originally blocked (and how it was fixed):**

1. **CUDA context creation.** The old code used `av_hwdevice_ctx_alloc()` +
   `av_hwdevice_ctx_init()` to create the CUDA context. On the current driver
   this produces a context that CUVID cannot push (`CUDA_ERROR_INVALID_VALUE` /
   `CUDA_ERROR_NOT_INITIALIZED`). The fix is to use `av_hwdevice_ctx_create()`
   and a `get_format` callback that selects `AV_PIX_FMT_CUDA`.

2. **Context thread-affinity.** `cuvidMapVideoFrame` is managed internally by
   FFmpeg's CUVID decoder. The old code called `cuCtxSetCurrent()` in the worker
   thread before decoding, which broke CUVID's internal push/pop and caused
   `CUDA_ERROR_OUT_OF_MEMORY` / `CUDA_ERROR_ILLEGAL_ADDRESS`. The fix is to let
   FFmpeg manage the decoder context and have the conversion kernel call
   `cudaSetDevice(0)` to activate the primary context only when it uses the
   returned CUDA pointers.

3. **`output_cuda` flag was reset before use.** `vmc_ffmpeg_decoder_init()` does
   a `memset()` of the decoder struct, so the old code that set
   `dec.output_cuda = true` before init silently lost the flag. The flag is now
   set after init and before `vmc_decoder_open()`.

Net result: the full GPU scanout path is now active, dropping decode latency to
~6.9 ms (vs ~11.5 ms in Design A) and providing true motion-to-photon timing.

### Display fixes for non-1920×1080 modes

The DRM connector is not always 1920×1080 (e.g. the LG monitor reports
1366×768 as its preferred mode). Three fixes keep the image correct at any
mode:

- `drm_scanout` picks the connector's **preferred / highest-resolution mode**
  instead of blindly `modes[0]`.
- The scanout copy is **row-by-row at the DRM dumb-buffer pitch** — a flat
  memcpy skewed every row whenever `pitch > width*4` (a "muffled" image).
- The latency overlay is **pitch-aware** (was drawn at packed offsets, smearing
  the text diagonally across the top of the screen).

## Audio downlink (ALSA)

The MEC stream carries PCM (UDP mode) or AAC (DASH mode) audio; the client
plays it through the monitor's HDMI output:

- **UDP mode**: `vmc-mec-sim` streams raw PCM (48 kHz stereo s16) as
  `STREAM_AUDIO` datagrams on a dedicated real-time thread (exact 200 pps,
  5 ms frames). The client feeds them straight to the ALSA sink.
- **DASH mode**: the client decodes AAC → FLTP → resamples to S16 48 kHz
  (version-guarded for FFmpeg 4.x vs 5+ channel-layout APIs) → ALSA.
- `src/audio/alsa_sink.c` — ALSA sink (48k/s16/stereo) with a silent fallback,
  plus an **HDMI unmute** (`alsa_unmute_hdmi()`): NVIDIA HDA outputs boot with
  the IEC958 playback switch off, so the sink turns it on at init.

The client device needs the user in the `audio` group and a working ALSA
configuration (the system config on this device can't resolve card indices, so
the service uses a minimal `/etc/vmc-audio.conf` and `VMC_AUDIO_DEV=hdmi`).

## GPU / display notes

- **Encoder:** NVENC (`h264_nvenc`). Bitrate auto-scales with resolution.
- **Decoder:** CUVID (`h264_cuvid`). NV12 output, `delay=0`, monotonic packet
  `pts` (required or CUVID holds frames at 4K).
- **`nvidia-drm` limitations (both confirmed by probes):**
  - `drmModeAddFB2` **rejects NV12** (EINVAL) despite the planes advertising
    it → scanout must use XRGB buffers.
  - CUDA cannot import the DRM/GBM dma-bufs (`cudaImportExternalMemory` fails)
    → the GPU conversion must write to CUDA-owned memory, then copy (or be
    bridged via EGL interop, not yet implemented).
- **`cudaMemcpyAsync` to unpinned memory is silently synchronous** (~1 ms
  staging) — pinned (`cudaHostAlloc`/`cudaHostRegister`) memory is required
  for true async DMA.

## Wi-Fi

The MediaTek MT7902 (Wi-Fi 6E) card needs a **driver built from source**
(mainline mt76 before the `aql_pending` API change). Modules are signed with
the machine's MOK key (Secure Boot) and installed to
`/lib/modules/$(uname -r)/updates/`. See `tools/wifi_connect.sh`:

```sh
sudo wifi_connect "SSID" "PASSWORD"
```

## Secure Boot

The NVIDIA driver and the custom mt76 modules are signed with the enrolled MOK
key; enrollment is done once via `mokutil --import` + reboot.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

Covers: ring buffer, protocol framing/CRC, jitter buffer (reorder, gaps,
duplicates), session state machine, audio mixer, input batching, fragment
assembler, and a real H.264 decode test (validates CUVID produces non-blank
pixels). The decoder test requires FFmpeg dev headers.

## Known issues / limitations

- **Direct DASH reader is now the default; old libavformat demuxer is fallback.**
  The FFmpeg DASH demuxer's live-edge computation was unreliable on this FFmpeg
  4.x build, causing it to request future/phantom segment numbers and then
  **block internally on the HTTP fetch** (ignoring `rw_timeout`) → a ~20–30 s
  freeze every ~60–100 s. The client now uses `dash_reader_direct` by default:
  a non-blocking `poll()`-based HTTP fetcher with Content-Length and chunked
  support, in-memory init + segment demuxing, and a 2-segment live-edge safety
  lag. The original periodic re-sync blips are gone. The old path is still
  available with `VMC_DASH_LIBAV=1`.
- **Encoder live pacing used to drift/hang.** `ffmpeg -re` + `-stream_loop -1`
  drifts to ~1.55× real-time over hours and eventually hangs in
  `hrtimer_nanosleep`; the lavfi `movie/amovie + loop + setpts` pacing was exact
  but the separate `loop=loop=0:size=N` filter exits at the end of the file on
  FFmpeg 4.x, forcing a restart every file length (~60 s for the demo clip).
  The dash-sim now uses `movie=file:loop=0,setpts=N/(fps*TB)` (and
  `amovie=…:loop=0`), which loops indefinitely with no restarts, and the
  encoder watchdog's stall check guards against unsigned underflow so it never
  spuriously restarts a healthy encoder.
- **Overnight jitter root cause** (debugged): the encoder froze for ~5 h while
  the client limped on a stale segment window at ~12 fps and the audio underran
  (ALSA XRUN). The lavfi loop fix + watchdog guards above prevent the permanent
  freeze and the speed-up; residual behaviour is brief periodic re-syncs rather
  than a hard stall.
- **DASH latency** ~1–3 s live-edge vs ~54 ms on UDP — the expected LL-DASH
  trade-off. DASH audio works; ABR (multi-representation) is not implemented.
- **No audio in DASH VOD-style playback path edge cases** — audio is verified
  in the UDP path and in DASH mode; A/V sync drift over long runs is not yet
  corrected (audio is paced by the ALSA clock, video by arrival).
- **HDMI audio requires setup** on the client: user in the `audio` group, a
  minimal ALSA config (`/etc/vmc-audio.conf`), and `VMC_AUDIO_DEV=hdmi`.
- **DRM mode** is the connector's preferred mode (1366×768 on this monitor),
  not forced to 1920×1080; the image renders correctly at any mode.
- **`-streaming 1` needed for `availabilityStartTime`**: the direct manifest
  fetch depends on it; plain `-use_timeline 1` without `-streaming` omits the
  wall-clock anchor.
- **Realtek/USB NICs on both machines have no DPDK PMD** — the earlier DPDK
  evaluation is blocked by hardware (no DPDK-capable NIC on the media path).

## Status / roadmap

- [x] Core + protocol + session state machine
- [x] Real H.264 path: NVENC → fragment → UDP → jitter → reassemble → CUVID → display
- [x] 4K transmission, decode-worker pipelining, on-screen latency overlay
- [x] GPU NV12→BGRA conversion (Design A, 6.7× faster than swscale)
- [x] Design B components: DRM scanout + flip events, stream-overlapped conversion
- [x] Wi-Fi driver (MT7902) + auto-connect, persistent services
- [x] Design B end-to-end: DRM scanout + CUVID CUDA frames + stream-overlapped conversion
- [x] C/CUDA MEC sender (`vmc-mec-sim`) replacing the Python sim (removes the
      ~50 ms GIL/stamping bias; e2e avg ~119 → ~66 ms)
- [x] Audio downlink: ALSA sink, UDP PCM streaming, HDMI IEC958 unmute
- [x] Display fixes: pitch-aware scanout + overlay, preferred-mode selection
- [x] LL-DASH transport: `vmc-dash-sim` live server + client `--dash` mode
      (video + AAC audio over HTTP, ~1–3 s live-edge)
- [x] Overnight-jitter hardening: lavfi loop pacing, encoder watchdog,
      client self-heal + reader watchdog
- [x] Fix the dash demuxer's live-timing (or land the direct segment-fetch
      client) to remove the periodic ~20–30 s re-sync blips
- [x] DASH playback quality: absolute live-edge math (rolled-window safe),
      frame-rate-paced presentation, DRM flip fixes, one-ahead audio buffering,
      `movie=:loop=0` endless encoder loop, watchdog underflow guard, optional
      `VMC_HUD=1` latency overlay
- [ ] DASH ABR (multi-representation MPD → the demuxer picks bitrate)
- [ ] A/V sync correction over long runs (align audio clock to video)
- [ ] E2E latency measurement refinement for Design B (PTS-to-send mapping with CUVID 1-frame hold)
- [ ] EGL interop or zero-copy decode-NV12 + DRM scanout
- [ ] Mapper protocol hardening + session migration
- [ ] Degraded-mode / offline fallback UI
