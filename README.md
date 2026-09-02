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
└──────────────────────┘                              │  → NV12 → BGRA conv   │
                                                      │  → /dev/fb0 → HDMI    │
                                                      └───────────────────────┘
```

Roles:

- **MEC sim** (`tools/mec_sim.py`) — stands in for the ReDroid/MEC container.
  Encodes real H.264 with **NVENC**, wraps access units in the VMC protocol
  (fragmenting large frames), echoes keepalives, and ingests input batches.
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
tools/        mec_sim.py, wifi_connect.sh, cuda/nv12_conv.cu
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

## Running

**1. Start the MEC sim** (on the machine with the encoder GPU):

```sh
python3 tools/mec_sim.py 192.168.0.126 9999 6000 [width] [height]
# e.g. 1920 1080, or 3840 2160 for 4K (~24 Mbps, NVENC)
```

**2. Run the thin client** (on the client device):

```sh
./build/vmc-thinclient-app <mapper-ip> <mapper-port> [log-level]
# e.g. ./build/vmc-thinclient-app 192.168.0.126 9999 1
```

A green overlay in the top-left shows live E2E / DEC / NET latency; full
stats + p95 are logged every 5 s.

**3. As persistent services** (systemd, on the client):

```sh
systemctl enable --now vmc-wifi.service       # auto-join Wi-Fi at boot
systemctl enable --now vmc-thinclient.service  # stream at boot
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

Representative results (Wi-Fi 6, 4K stream → 1080p display):

| Metric | Value | Notes |
|---|---|---|
| e2e avg | ~122 ms | includes ~60 ms sim-Python-GIL bias + ~45 ms CUVID 1-frame hold |
| decode avg | ~11.5 ms | CUVID + NV12→BGRA conversion |
| one-way | ~2–3 ms | WiFi |
| queue | ~25 µs | decode-worker pipeline |

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

### 3. GPU scanout — Design B / Option 1 (implemented, experimental, BLOCKED)
The full-GPU path: CUVID → CUDA frames → stream-overlapped conversion →
DRM page-flip scanout. Opt-in with `VMC_DRM=1`. Components (all built and
validated individually):

- **`drm_scanout.c`** — 3 dumb XRGB buffers, `drmModePageFlip` with
  `DRM_EVENT_FLIP_COMPLETE`, per-buffer flip timestamps → the host CPU gets
  **vsync-accurate "on screen" timing** (motion-to-photon).
- **Stream-overlapped conversion** (`conv_async`/`conv_wait_event`) — a
  dedicated CUDA stream + **pinned staging**; the queue returns in ~0.04 ms and
  the conversion (~1.5 ms) overlaps with the next frame's decode. Verified in a
  standalone probe: ~0.9 ms CPU-visible per frame, ~0.05 ms queue.
- **`output_format=cuda` decoder mode** — keeps NV12 on the GPU.

**Why Design B is blocked (root cause, in detail):**

The enabling piece — CUVID's `output_format=cuda` (delivering decoded frames
as CUDA device pointers via `cuvidMapVideoFrame`) — **fails on this
driver/FFmpeg combination**, so the client cannot run the full GPU path. The
failure chain:

1. **CUDA context thread-affinity.** `cuvidMapVideoFrame` maps a NVDEC surface
   into the *calling thread's current CUDA context*. CUDA contexts are
   thread-affine: a context created/current in one thread must be made current
   in any thread that uses it. Our decoder opens in the main thread but
   decodes in a worker pthread. Without binding the context in the worker,
   the map targets the wrong/no context → `CUDA_ERROR_ILLEGAL_ADDRESS`.

2. **Fixing the context isn't enough.** We provide our own CUDA device context
   (`hw_device_ctx`) and bind it in the worker via `cuCtxSetCurrent`. But with
   a `hw_device_ctx` set, FFmpeg switches to its generic CUDA hwaccel frame
   delivery, which performs `cuMemcpy2DAsync` into a `hw_frames_ctx` pool —
   and that also fails with `CUDA_ERROR_ILLEGAL_ADDRESS` (and
   `cuvidMapVideoFrame` with `CUDA_ERROR_OUT_OF_MEMORY`). The full-decoder
   `output_format=cuda` path and the hw_frames_ctx machinery conflict on this
   driver.

3. **Why the CLI works but the API path doesn't.** `ffmpeg -c:v h264_cuvid`
   decodes single-threaded with FFmpeg's internal CUDA machinery (its own
   context, same thread). Driving the decoder through the raw `avcodec` API
   from a separate thread trips the same calls with none of the CLI's setup.

Net result: the cuvid-CUDA-frame delivery is the sole blocker; the conversion
overlap and DRM scanout (the parts we built) are validated and working.

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
- [ ] Design B end-to-end (blocked: cuvid `output_format=cuda` on this driver;
      next: EGL interop or decode-NV12 + DRM scanout)
- [ ] Audio I/O (ALSA) + full mixer
- [ ] Mapper protocol hardening + session migration
- [ ] Degraded-mode / offline fallback UI
