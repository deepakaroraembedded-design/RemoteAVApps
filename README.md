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

## Repo layout

```
include/vmc/
  core/       types, error codes, logger, ring buffer, platform/time
  session/    session lifecycle + state machine, mapper client
  transport/  wire protocol, jitter buffer, UDP transport
  video/      decoder/display interfaces, FFmpeg decoder, fragment assembler,
              fb0 backend, DRM scanout backend
  input/      input abstraction, Linux evdev backend, input batching
  audio/      MEC downlink + local mix pipeline
src/          implementations (platform/linux holds Linux-specific code)
apps/         thin client main application (decode-worker pipeline + overlay)
tests/        unit tests (8 suites, dependency-free harness)
tools/        vmc_sim/ (C/CUDA MEC sender), mec_sim.py (legacy Python sim),
              wifi_connect.sh, cuda/nv12_conv.cu
systemd/      vmc-thinclient.service, vmc-wifi.service
```

## Building

Dependencies: CMake, a C11 compiler. Optional, auto-detected:
- **FFmpeg** dev headers (`libavcodec-dev libswscale-dev`) → H.264 decoder
- **libdrm** dev headers → DRM scanout backend
- **CUDA** headers (`cuda.h`) → CUVID CUDA device-context path

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

`vmc-mec-sim` is built as part of the main build (target
`tools/vmc_sim/vmc-mec-sim`, linked against `libvmc_thinclient.a`).

## Running

**1. Start the MEC sim** (on the machine with the encoder GPU):

```sh
./build/tools/vmc_sim/vmc-mec-sim 192.168.0.126 9999 6000 [width] [height] [drop_rate]
# e.g. 1920 1080, or 3840 2160 for 4K (~24 Mbps, NVENC)
# drop_rate defaults to 0.001 (matches the Python sim); pass 0.0 to disable drops
```

The legacy Python sim is still available as `tools/mec_sim.py` with the same
argument order.

**2. Run the thin client** (on the client device):

```sh
./build/vmc-thinclient-app <mapper-ip> <mapper-port> [log-level]
# e.g. ./build/vmc-thinclient-app 192.168.0.126 9999 1
```

To enable the GPU scanout path (Design B) instead of the default framebuffer
path, set `VMC_DRM=1`:

```sh
VMC_DRM=1 ./build/vmc-thinclient-app 192.168.0.126 9999 1
```

A green overlay in the top-left shows live E2E / DEC / NET latency; full
stats + p95 are logged every 5 s.

**3. As persistent services** (systemd, on the client):

Service files are in `systemd/`. Copy them to `/etc/systemd/system/`, edit the
user, home-directory, and Wi-Fi interface (`wlp6s0`) as needed, then:

```sh
sudo cp systemd/*.service /etc/systemd/system/
sudo systemctl daemon-reload
systemctl enable --now vmc-wifi.service       # auto-join Wi-Fi at boot
systemctl enable --now vmc-thinclient.service  # stream at boot (VMC_DRM=1)
```

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
- [ ] E2E latency measurement refinement for Design B (PTS-to-send mapping with CUVID 1-frame hold)
- [ ] EGL interop or zero-copy decode-NV12 + DRM scanout
- [ ] Audio I/O (ALSA) + full mixer
- [ ] Mapper protocol hardening + session migration
- [ ] Degraded-mode / offline fallback UI
