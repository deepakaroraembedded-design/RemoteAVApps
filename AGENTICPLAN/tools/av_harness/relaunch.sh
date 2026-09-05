#!/usr/bin/env bash
# CLEAN-SLATE relaunch of the LL-DASH server (192.168.0.126) and the thin
# client (192.168.0.145). Kills, VERIFIES DEAD, purges, starts, VERIFIES UP.
#
# Exit codes:
#   0  both up and healthy
#  10  client did not die (stale instance holds the HDMI PCM)
#  11  server did not die / port 8080 still bound
#  12  server came up but the MPD never went live
#  13  client came up but produced no telemetry
#  14  unsafe DASH_OUT (refused to purge)
#  15  ssh unreachable
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
. "$HERE/env.sh"
R="$HERE/remote.sh"

log() { printf '[relaunch %s] %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }

# ---------------------------------------------------------------- guardrail
case "$DASH_OUT" in
  /tmp/*|/var/tmp/*) : ;;
  *) log "REFUSING to purge DASH_OUT=$DASH_OUT (must be under /tmp or /var/tmp)"; exit 14 ;;
esac

# ---------------------------------------------------------------- 0. reachable
"$R" --quiet 'true' || { log "client $CLIENT_IP unreachable over ssh"; exit 15; }

# ---------------------------------------------------------------- 1. kill client
log "killing client instances"
"$R" 'sudo pkill -9 -f vmc-thinclient-app || true' >/dev/null 2>&1
for i in $(seq 1 20); do
  n="$("$R" 'pgrep -c -f vmc-thinclient-app || true' 2>/dev/null | tr -d "[:space:]")"
  [ -z "$n" ] && n=0
  [ "$n" = "0" ] && break
  sleep 0.5
done
[ "${n:-1}" = "0" ] || { log "client still running after 10s (n=$n)"; exit 10; }

# The README failure mode: a leftover client holds the HDMI PCM and the next
# instance silently degrades to a silent sink. Verify the PCM is released.
for i in $(seq 1 10); do
  st="$("$R" "cat $ALSA_PCM_STATUS 2>/dev/null | head -1" 2>/dev/null || true)"
  case "$st" in *RUNNING*) sleep 0.5 ;; *) break ;; esac
done
case "${st:-}" in *RUNNING*) log "HDMI PCM still RUNNING - stale audio holder"; exit 10 ;; esac

# ---------------------------------------------------------------- 2. kill server
log "killing server instances"
pkill -9 -f 'vmc-dash-sim' >/dev/null 2>&1 || true
pkill -9 -f 'ffmpeg.*dash' >/dev/null 2>&1 || true
for i in $(seq 1 20); do
  pgrep -f 'vmc-dash-sim' >/dev/null 2>&1 || break
  sleep 0.5
done
pgrep -f 'vmc-dash-sim' >/dev/null 2>&1 && { log "server still running after 10s"; exit 11; }
for i in $(seq 1 20); do
  ss -tln 2>/dev/null | grep -q ":${DASH_PORT} " || break
  sleep 0.5
done
ss -tln 2>/dev/null | grep -q ":${DASH_PORT} " && { log "port $DASH_PORT still bound"; exit 11; }

# ---------------------------------------------------------------- 3. purge
log "purging segments and logs"
mkdir -p "$DASH_OUT"
rm -f "$DASH_OUT"/*.m4s "$DASH_OUT"/*.mpd "$DASH_OUT"/*.tmp 2>/dev/null || true
: > "$SERVER_LOG"; : > "$SERVER_TLM"
"$R" ": > $CLIENT_LOG; : > $CLIENT_TLM" >/dev/null 2>&1

# ---------------------------------------------------------------- 4. start server
# Single-stretch workload: the clip is played ONCE, end to end. --play-once
# makes the encoder use movie=...:loop=1 (play once) instead of loop=0 (loop
# forever) and marks the stream ended at EOS instead of letting the watchdog
# mistake end-of-content for a stall and respawn the encoder mid-test.
PLAY_ONCE_ARG=""
[ "$PLAY_ONCE" = "1" ] && PLAY_ONCE_ARG="--play-once"

log "checking the clip is the expected length"
if command -v ffprobe >/dev/null 2>&1; then
  dur="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$INPUT_CLIP" 2>/dev/null | cut -d. -f1)"
  if [ -n "$dur" ]; then
    diff=$(( dur > CLIP_DURATION_S ? dur - CLIP_DURATION_S : CLIP_DURATION_S - dur ))
    [ "$diff" -gt 5 ] && log "WARNING: clip is ${dur}s, CLIP_DURATION_S=$CLIP_DURATION_S"
  fi
fi

log "starting vmc-dash-sim (play_once=$PLAY_ONCE)"
VMC_TELEMETRY="$SERVER_TLM" setsid nohup \
  "$SERVER_BUILD/tools/vmc-dash-sim/vmc-dash-sim" \
  --input "$INPUT_CLIP" --port "$DASH_PORT" --log-level 1 $PLAY_ONCE_ARG \
  > "$SERVER_LOG" 2>&1 < /dev/null &
disown || true

# Live when the MPD parses AND startNumber advances twice (proves the rolling
# window is actually turning, not just that a file exists).
sn_prev=""; advances=0
for i in $(seq 1 60); do
  sleep 0.5
  mpd="$(curl -s --max-time 2 "http://127.0.0.1:${DASH_PORT}/live.mpd" 2>/dev/null || true)"
  case "$mpd" in *availabilityStartTime*) : ;; *) continue ;; esac
  sn="$(printf '%s' "$mpd" | grep -o 'startNumber="[0-9]*"' | head -1 | grep -o '[0-9]*' || true)"
  [ -z "$sn" ] && continue
  if [ -n "$sn_prev" ] && [ "$sn" != "$sn_prev" ]; then advances=$((advances+1)); fi
  sn_prev="$sn"
  [ "$advances" -ge 2 ] && break
done
[ "$advances" -ge 2 ] || { log "MPD never went live (advances=$advances)"; exit 12; }
log "server live (startNumber=$sn_prev)"

# ---------------------------------------------------------------- 5. start client
log "starting vmc-thinclient-app"
"$R" "setsid nohup env VMC_DRM=$VMC_DRM VMC_AUDIO_DEV=$VMC_AUDIO_DEV \
      ALSA_CONFIG_PATH=$VMC_ALSA_CONF VMC_TELEMETRY=$CLIENT_TLM \
      $CLIENT_BUILD/vmc-thinclient-app --dash $MPD_URL 1 \
      > $CLIENT_LOG 2>&1 < /dev/null & disown" >/dev/null 2>&1

# Healthy when the first vrender event lands: decode + scanout are both alive.
ok=0
for i in $(seq 1 40); do
  sleep 0.5
  if "$R" --quiet "grep -q '\"t\":\"vrender\"' $CLIENT_TLM"; then ok=1; break; fi
  # fatal decoder faults abort early rather than burning the full 20s
  if "$R" --quiet "grep -q 'decoder_unavailable' $CLIENT_TLM"; then
    log "decoder_unavailable at startup"; exit 13
  fi
done
[ "$ok" = "1" ] || { log "no vrender telemetry within 20s"; exit 13; }

log "clean slate up: server=$SERVER_IP:$DASH_PORT client=$CLIENT_IP"
exit 0
