#!/usr/bin/env bash
# Single source of truth for the VMC A/V agentic loop.
# Target workload: ONE 21-minute clip played end-to-end, in a single stretch.
# Source it: . tools/av_harness/env.sh
# Override anything by exporting it before sourcing.

# ---------- topology ----------
export SERVER_IP="${SERVER_IP:-192.168.0.126}"
export CLIENT_IP="${CLIENT_IP:-192.168.0.145}"
export CLIENT_USER="${CLIENT_USER:-deepak7121}"
export CLIENT_SSH_KEY="${CLIENT_SSH_KEY:-$HOME/.ssh/vmc_agent}"
export DASH_PORT="${DASH_PORT:-8080}"
export MPD_URL="${MPD_URL:-http://${SERVER_IP}:${DASH_PORT}/live.mpd}"

# ---------- paths ----------
# Resolve to the git top-level so the harness works whether the bundle lives at
# the repo root (as the plan intends) or nested under AGENTICPLAN/ during bring-up.
export REPO="${REPO:-$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd))}"
export CLIENT_REPO="${CLIENT_REPO:-/home/${CLIENT_USER}/vmc-thinclient}"
export SERVER_BUILD="${SERVER_BUILD:-$REPO/build-server-debug}"
export CLIENT_BUILD="${CLIENT_BUILD:-$CLIENT_REPO/build-debug}"

# THE 21-MINUTE CLIP. Played ONCE, start to finish. Never edited by any agent.
# Sourced once (FROZEN, sha256 48304bd8eef908ddf4b28c20e5817b5423dd962eb2955f00ea676eeb39932fc3)
# from 4K/Earth.mp4: ffmpeg -ss 0 -t 1260 -i 4K/Earth.mp4 -c copy -avoid_negative_ts make_zero
# Native 1920x1080 @60 fps H.264 + AAC 44.1 kHz stereo — content is NOT re-encoded, so the
# pipeline is validated at a higher cadence than the 24 fps the code was originally tuned for.
export INPUT_CLIP="${INPUT_CLIP:-$REPO/4K/swiftrade_av_21min.mp4}"
export CLIP_DURATION_S="${CLIP_DURATION_S:-1260}"     # 21:00
export PLAY_ONCE="${PLAY_ONCE:-1}"                    # encoder must NOT loop

# Segment output dir purged on every clean-slate relaunch.
# MUST live under /tmp or /var/tmp - relaunch.sh refuses to delete anywhere else.
export DASH_OUT="${DASH_OUT:-/tmp/vmc_dash_out}"
export DASH_OUT_MIN_FREE_MB="${DASH_OUT_MIN_FREE_MB:-4096}"   # 21 min needs headroom

export SERVER_LOG="${SERVER_LOG:-/tmp/dash_server.log}"
export CLIENT_LOG="${CLIENT_LOG:-/tmp/dash_client.log}"
export SERVER_TLM="${SERVER_TLM:-/tmp/vmc_tlm_server.ndjson}"
export CLIENT_TLM="${CLIENT_TLM:-/tmp/vmc_tlm_client.ndjson}"
export CLIENT_TLM_MIN_FREE_MB="${CLIENT_TLM_MIN_FREE_MB:-1024}"  # ~60 MB/run + slack

export RUNS_DIR="${RUNS_DIR:-$REPO/runs}"

# ---------- client runtime env ----------
export VMC_DRM="${VMC_DRM:-1}"
export VMC_AUDIO_DEV="${VMC_AUDIO_DEV:-hdmi}"
export VMC_ALSA_CONF="${VMC_ALSA_CONF:-/etc/vmc-audio.conf}"
# ALSA PCM status path checked by relaunch/collect for "audio actually
# playing". The client routes audio to a physically-attached USB headset when
# one is present (its PCM goes RUNNING, the HDMI PCM does not), so the check
# must point at the USB playback PCM on THE CLIENT in that case, else the HDMI
# status stays idle and the precheck falsely fails. Resolved remotely from the
# client's /proc/asound/cards; falls back to the HDMI PCM.
_alsa_pcm_status() {
  local r=""
  if [ -n "${CLIENT_IP:-}" ] && command -v ssh >/dev/null 2>&1; then
    r="ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new ${CLIENT_USER}@${CLIENT_IP}"
    [ -f "$CLIENT_SSH_KEY" ] && r="ssh -i $CLIENT_SSH_KEY -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new ${CLIENT_USER}@${CLIENT_IP}"
  fi
  if [ -n "$r" ]; then
    local path
    path="$($r 'while read -r num rest; do case "$rest" in *USB-Audio*) [ -f /proc/asound/card${num}/pcm0p/sub0/status ] && { echo /proc/asound/card${num}/pcm0p/sub0/status; exit 0; } ;; esac; done < /proc/asound/cards; echo /proc/asound/card1/pcm3p/sub0/status' 2>/dev/null)"
    [ -n "$path" ] && { printf '%s' "$path"; return; }
  fi
  printf '/proc/asound/card1/pcm3p/sub0/status'
}
export ALSA_PCM_STATUS="${ALSA_PCM_STATUS:-$( _alsa_pcm_status )}"

# ---------- measurement window ----------
# Two tiers. SMOKE is a cheap pre-filter so a gross fault does not cost 21
# minutes of wall clock; FULL is the real test and the only thing that can PASS.
export SMOKE_S="${SMOKE_S:-180}"                      # 3 min pre-filter
export FULL_S="${FULL_S:-1235}"                       # 1260 - warmup - tail
export WARMUP_S="${WARMUP_S:-15}"                     # discarded: prefill, first resync
export TAIL_EXCLUDE_S="${TAIL_EXCLUDE_S:-10}"         # discarded: EOS drain
export BUCKET_S="${BUCKET_S:-60}"                     # per-minute statistics buckets

export CONTENT_FPS="${CONTENT_FPS:-60.0}"            # Earth.mp4 native rate (24 fps legacy)
export AUDIO_RATE="${AUDIO_RATE:-48000}"             # the client's ALSA output rate (the AAC
                                                      # content is 44100 but is resampled to 48k
                                                      # before the sink, so the render cadence is
                                                      # 48k — the period-deviation gate must count
                                                      # the OUTPUT periods, not the content rate)
export AUDIO_PERIOD="${AUDIO_PERIOD:-240}"           # 5 ms ALSA period (960 B stereo s16 @48k)
export AUDIO_FIFO_BYTES="${AUDIO_FIFO_BYTES:-2097152}" # client audio FIFO (2 MiB)
export SEG_DURATION_S="${SEG_DURATION_S:-1.0}"
# Presentation cadence reference for the frame-interval gate. At 60 fps content
# the per-frame period is 16.667 ms (not the 41.667 ms the 24 fps pipeline was
# tuned against) — the client paces to the demuxer/manifest frame rate.
export FRAME_INTERVAL_MS="$(awk "BEGIN{printf \"%.3f\", 1000/$CONTENT_FPS}")"

# In-run liveness heartbeat: abort a doomed 21-minute run early instead of
# waiting it out. Checks process alive + telemetry growing.
export HEARTBEAT_S="${HEARTBEAT_S:-30}"
export HEARTBEAT_STALL_LIMIT="${HEARTBEAT_STALL_LIMIT:-3}"   # 3 x 30s = 90s dead -> abort

# Resource sampling cadence (leak detection over a long run)
export RESSAMPLE_S="${RESSAMPLE_S:-60}"

# ---------- timeouts (seconds) ----------
export SSH_TIMEOUT="${SSH_TIMEOUT:-120}"
export RELAUNCH_TIMEOUT="${RELAUNCH_TIMEOUT:-90}"
# collect must outlast the full run: warmup + window + tail + slack
export COLLECT_TIMEOUT="${COLLECT_TIMEOUT:-1500}"

# ---------- PASS gates ----------
# Applied to EVERY complete 60 s bucket AND to the run as a whole. A single bad
# minute out of 21 fails the run - aggregate-only gating hides transients, which
# is the entire reason for testing a long single stretch.

# A/V sync. EBU R37 envelope: audio may lead +40 ms, lag -60 ms.
export GATE_AV_OFFSET_MEAN_ABS_MS="${GATE_AV_OFFSET_MEAN_ABS_MS:-10.0}"
export GATE_AV_OFFSET_P95_ABS_MS="${GATE_AV_OFFSET_P95_ABS_MS:-20.0}"
export GATE_AV_OFFSET_MAX_MS="${GATE_AV_OFFSET_MAX_MS:-40.0}"     # video ahead
export GATE_AV_OFFSET_MIN_MS="${GATE_AV_OFFSET_MIN_MS:--60.0}"    # video behind
# The envelope is gated as a RATE, not on absolute min/max: over 30,240 frames a
# single outlier is noise, while a real glitch puts hundreds of frames outside.
# 0.1% of a 60 s bucket = ~1.4 frames.
export GATE_AV_ENVELOPE_VIOLATION_PCT="${GATE_AV_ENVELOPE_VIOLATION_PCT:-0.1}"

# Long-run drift gates. 0.5 ms/min x 21 min = ~10 ms total - the whole point of
# a 21-minute stretch is that a drift invisible in 60 s becomes decisive here.
export GATE_AV_DRIFT_ABS_MS_PER_MIN="${GATE_AV_DRIFT_ABS_MS_PER_MIN:-0.5}"
export GATE_AV_TOTAL_EXCURSION_MS="${GATE_AV_TOTAL_EXCURSION_MS:-15.0}"
export GATE_AV_P95_DEGRADE_MS_PER_BUCKET="${GATE_AV_P95_DEGRADE_MS_PER_BUCKET:-0.3}"

# Cadence (per bucket). interval reference is FRAME_INTERVAL_MS (16.667 ms @60 fps,
# 41.667 ms @24 fps); the p95 error tolerance scales with it via GATE_FRAME_INTERVAL_FRAC.
export GATE_FRAME_YIELD_MIN="${GATE_FRAME_YIELD_MIN:-0.995}"
export GATE_FRAME_INTERVAL_FRAC="${GATE_FRAME_INTERVAL_FRAC:-0.10}"   # 10% of the period
export GATE_AUDIO_PERIOD_TOLERANCE="${GATE_AUDIO_PERIOD_TOLERANCE:-0.005}"

# Buffers
export GATE_AUDIO_FIFO_LEVEL_MIN_PCT="${GATE_AUDIO_FIFO_LEVEL_MIN_PCT:-15.0}"

# Network
export GATE_SEG_FETCH_P95_MS="${GATE_SEG_FETCH_P95_MS:-800.0}"
export GATE_SEG_FETCH_MAX_MS="${GATE_SEG_FETCH_MAX_MS:-2000.0}"
export GATE_TCP_RTT_P95_MS="${GATE_TCP_RTT_P95_MS:-30.0}"
export GATE_RESYNCS_MAX="${GATE_RESYNCS_MAX:-1}"      # run-level, not per bucket

# Decoder
export GATE_DECODE_P95_MS="${GATE_DECODE_P95_MS:-15.0}"

# Resource leaks - only observable over a long stretch
export GATE_RSS_GROWTH_MB="${GATE_RSS_GROWTH_MB:-32.0}"
export GATE_FD_GROWTH="${GATE_FD_GROWTH:-8}"
export GATE_DISK_GROWTH_MB="${GATE_DISK_GROWTH_MB:-512.0}"
export GATE_DISK_FLAT_LAST_BUCKETS="${GATE_DISK_FLAT_LAST_BUCKETS:-10}"

# Completion - the run must actually reach the end of the clip
export GATE_REQUIRE_EOS="${GATE_REQUIRE_EOS:-1}"
export GATE_EOS_TOLERANCE_S="${GATE_EOS_TOLERANCE_S:-5.0}"

# Presentation latency (NEW fault class). Live-edge lag = wall-clock at vblank minus
# the content's realtime timestamp (from clock_sync pairing + srv/mpd_update
# avail_start_us). NOTE: the client's dynamic live buffer runs 10 s (steady) to 30 s
# (max), so a HEALTHY present-delay is ~10 s — absolute mean is buffer-config dependent
# and is reported but NOT gated. The two gates that matter are:
#   p95 ceiling  - above the 30 s max buffer + slack => the reader permanently lost
#                  the live edge while A/V stayed locked (invisible to av_sync).
#   growth       - Theil-Sen over per-bucket mean lag; robust to buffer re-aiming
#                  steps (which are by design), catches monotonic lateness.
export GATE_PRESENT_DELAY_P95_MS="${GATE_PRESENT_DELAY_P95_MS:-30000.0}"
export GATE_PRESENT_DELAY_GROWTH_MS_PER_MIN="${GATE_PRESENT_DELAY_GROWTH_MS_PER_MIN:-10.0}"

# Everything not listed above is a zero-tolerance counter (drops, xruns,
# overflows, underflows, vsync/async faults, decoder errors, fetch failures).

av_env_dump() {
  echo "REPO=$REPO"
  echo "server=$SERVER_IP:$DASH_PORT  client=$CLIENT_USER@$CLIENT_IP"
  echo "clip=$INPUT_CLIP  duration=${CLIP_DURATION_S}s  play_once=$PLAY_ONCE"
  echo "content=${CONTENT_FPS}fps  audio=${AUDIO_RATE}Hz  frame_interval=${FRAME_INTERVAL_MS}ms"
  echo "smoke=${SMOKE_S}s  full=${FULL_S}s  warmup=${WARMUP_S}s  tail=${TAIL_EXCLUDE_S}s"
  echo "buckets=$((FULL_S / BUCKET_S)) x ${BUCKET_S}s @ ${CONTENT_FPS}fps"
  echo "gates/bucket: av_mean<=${GATE_AV_OFFSET_MEAN_ABS_MS}ms p95<=${GATE_AV_OFFSET_P95_ABS_MS}ms"
  echo "gates/run:    drift<=${GATE_AV_DRIFT_ABS_MS_PER_MIN}ms/min" \
       "excursion<=${GATE_AV_TOTAL_EXCURSION_MS}ms rss<=+${GATE_RSS_GROWTH_MB}MB eos=$GATE_REQUIRE_EOS"
  echo "gates/latency: present_delay p95<=${GATE_PRESENT_DELAY_P95_MS}ms" \
       "growth<=${GATE_PRESENT_DELAY_GROWTH_MS_PER_MIN}ms/min"
}
