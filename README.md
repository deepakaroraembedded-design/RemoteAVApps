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
│  → fragment → send   │                              │  CUVID decode (RTX3050)│
└──────────────────────┘                              │  swscale NV12→BGRA    │
                                                      │  → /dev/fb0 → HDMI    │
                                                      └───────────────────────┘
```

Roles:

- **MEC sim** (`tools/mec_sim.py`) — stands in for the ReDroid/MEC container.
  It encodes real H.264 with **NVENC**, wraps access units in the VMC protocol
  (fragmenting large frames), echoes keepalives, and ingests input batches.
- **Thin client** (`apps/thinclient/main.c`) — receives, reorders, reassembles,
  decodes with **CUVID** (software fallback), converts to RGB32, and presents
  to the framebuffer/HDMI. A decode worker thread keeps reception independent
  of decode cost.

## Repo layout

```
include/vmc/
  core/       types, error codes, logger, ring buffer, platform/time
  session/    session lifecycle + state machine, mapper client
  transport/  wire protocol, jitter buffer, UDP transport
  video/      decoder/display interfaces, FFmpeg decoder, fragment assembler,
              framebuffer display backend
  input/      input abstraction, Linux evdev backend, input batching
  audio/      MEC downlink + local mix pipeline
src/          implementations (platform/linux holds Linux-specific code)
apps/         thin client main application (decode-worker pipeline + overlay)
tests/        unit tests (8 suites, dependency-free harness)
tools/        mec_sim.py, wifi_connect.sh
```

## Building

Dependencies: CMake, a C11 compiler. Optional (auto-detected): FFmpeg dev
headers (`libavcodec-dev libswscale-dev`) to enable the H.264 decoder.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

With sanitizers:

```sh
cmake -S . -B build-asan -DVMC_ENABLE_SANITIZERS=ON
cmake --build build-asan
```

Build flags: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.

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

A green overlay in the top-left of the display shows live E2E / DEC / NET
latency (updates every frame); full stats + p95 are logged every 5 s.

**3. As persistent services** (systemd, installed on the client):

```sh
systemctl enable --now vmc-wifi.service      # auto-join Wi-Fi at boot
systemctl enable --now vmc-thinclient.service # stream at boot
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

Representative results (LAN, Wi-Fi 6):

| Resolution | E2E avg | p95 | one-way | decode | notes |
|---|---|---|---|---|---|
| 1080p30 | ~17 ms | 21 ms | ~1 ms | 4.7 ms | CPU NV12→BGRA |
| 4K30 | ~120 ms* | 127 ms | 2–3 ms | 13 ms | includes 1-frame hold (~45 ms) + sim GIL bias (~60 ms) |

*The 4K figure includes ~60 ms of measurement bias from the sim's Python
keepalive echo delay; true client-pipeline cost is ~60–65 ms.

## GPU notes

- **Encoder:** NVENC (`h264_nvenc`) on the MEC GPU. Bitrate auto-scales with
  resolution (~0.1 bit/px/frame).
- **Decoder:** CUVID (`h264_cuvid`, `output_format=nv12`, low-delay, monotonic
  packet `pts` — the pts are required or CUVID holds frames at 4K).
- **Display conversion:** the framebuffer is XRGB8888, so the decoder's NV12 is
  converted with `swscale`. Direct NV12 hardware-plane scanout is **not
  possible on nvidia-drm** (its `AddFB2` rejects NV12 with EINVAL despite the
  planes advertising it; XRGB works). GPU-side conversion would require
  NVIDIA's EGLStreams or a CUDA kernel.

## Wi-Fi

The client's MediaTek MT7902 (Wi-Fi 6E) card needs a **driver built from
source** (mainline mt76 before the `aql_pending` API change). Modules are
signed with the machine's MOK key (Secure Boot) and installed to
`/lib/modules/$(uname -r)/updates/`. See `tools/wifi_connect.sh` to join an AP:

```sh
sudo wifi_connect "SSID" "PASSWORD"
```

## Secure Boot

Both the NVIDIA driver and the custom mt76 modules are signed with the
enrolled MOK key (`/var/lib/shim-signed/mok/MOK.der`); enrollment is done once
via `mokutil --import` + reboot.

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
- [x] Wi-Fi driver (MT7902) + auto-connect, persistent services
- [ ] Audio I/O (ALSA) + full mixer
- [ ] Mapper protocol hardening + session migration
- [ ] GPU-side color conversion (CUDA/EGLStreams) to remove swscale
- [ ] Degraded-mode / offline fallback UI
