#!/usr/bin/env bash
# Measurement collector for the av-test-runner subagent.
# Workload: ONE 21-minute clip, played end to end, in a single stretch.
#
#   collect.sh --precheck
#   collect.sh --tier smoke          # 3 min pre-filter
#   collect.sh --tier full           # the real 21 min run
#   collect.sh --tier full --out RUN_DIR
#
# Assumes the server and client are ALREADY RUNNING (relaunch.sh ran first).
# Never starts, stops, or repairs anything.
#
# Exit codes: 0 ok | 20 precheck failed | 21 window truncated
#             22 no telemetry | 23 aborted early (client died mid-run)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
. "$HERE/env.sh"
R="$HERE/remote.sh"

log() { printf '[collect %s] %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }

MODE="run"; TIER="full"; OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --precheck) MODE="precheck"; shift ;;
    --tier)     TIER="$2"; shift 2 ;;
    --out)      OUT="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

case "$TIER" in
  smoke) WIN="$SMOKE_S" ;;
  full)  WIN="$FULL_S" ;;
  *) echo "tier must be smoke|full" >&2; exit 2 ;;
esac

# ------------------------------------------------------------------ precheck
precheck() {
  local fail=0

  "$R" --quiet 'true' || { echo "PRECHECK: ssh to $CLIENT_IP failed"; return 1; }

  local nc
  nc="$("$R" 'pgrep -c -f vmc-thinclient-app || true' 2>/dev/null | tr -d "[:space:]")"
  [ -z "$nc" ] && nc=0
  if [ "$nc" != "1" ]; then
    echo "PRECHECK: expected exactly 1 vmc-thinclient-app, found $nc"
    fail=1   # >1 is the silent-sink trap; 0 means it died
  fi

  local ns
  ns="$(pgrep -c -f vmc-dash-sim 2>/dev/null || true)"; [ -z "$ns" ] && ns=0
  if [ "$ns" != "1" ]; then
    echo "PRECHECK: expected exactly 1 vmc-dash-sim, found $ns"; fail=1
  fi

  local sn1 sn2
  sn1="$(curl -s --max-time 3 "$MPD_URL" | grep -o 'startNumber="[0-9]*"' | head -1 | grep -o '[0-9]*' || true)"
  sleep 2
  sn2="$(curl -s --max-time 3 "$MPD_URL" | grep -o 'startNumber="[0-9]*"' | head -1 | grep -o '[0-9]*' || true)"
  if [ -z "$sn1" ] || [ -z "$sn2" ]; then
    echo "PRECHECK: MPD did not parse at $MPD_URL"; fail=1
  elif [ "$sn1" = "$sn2" ]; then
    echo "PRECHECK: MPD startNumber not advancing ($sn1) - encoder stalled?"; fail=1
  fi

  local b1 b2
  b1="$("$R" "stat -c %s $CLIENT_TLM 2>/dev/null || echo 0" | tr -d '[:space:]')"
  sleep 2
  b2="$("$R" "stat -c %s $CLIENT_TLM 2>/dev/null || echo 0" | tr -d '[:space:]')"
  if [ "${b2:-0}" -le "${b1:-0}" ]; then
    echo "PRECHECK: client telemetry not growing ($b1 -> $b2) - VMC_TELEMETRY unset?"; fail=1
  fi

  local st
  st="$("$R" "cat $ALSA_PCM_STATUS 2>/dev/null | head -1" || true)"
  case "$st" in
    *RUNNING*) : ;;
    *) echo "PRECHECK: ALSA PCM not RUNNING (got: ${st:-<none>})"; fail=1 ;;
  esac

  # A 21-minute run writes ~60 MB of telemetry on the client and up to a few
  # hundred MB of segments on the host. Running out of disk at minute 17 is an
  # expensive way to discover a full /tmp.
  local free_srv free_cli
  free_srv="$(df -Pm "$(dirname "$DASH_OUT")" 2>/dev/null | awk 'NR==2{print $4}')"
  if [ -n "$free_srv" ] && [ "$free_srv" -lt "$DASH_OUT_MIN_FREE_MB" ]; then
    echo "PRECHECK: only ${free_srv}MB free for $DASH_OUT (need $DASH_OUT_MIN_FREE_MB)"; fail=1
  fi
  free_cli="$("$R" "df -Pm \$(dirname $CLIENT_TLM) | awk 'NR==2{print \$4}'" 2>/dev/null | tr -d '[:space:]')"
  if [ -n "$free_cli" ] && [ "$free_cli" -lt "$CLIENT_TLM_MIN_FREE_MB" ]; then
    echo "PRECHECK: only ${free_cli}MB free on client for telemetry (need $CLIENT_TLM_MIN_FREE_MB)"; fail=1
  fi

  return $fail
}

if [ "$MODE" = "precheck" ]; then
  if precheck; then log "precheck OK"; exit 0; else log "precheck FAILED"; exit 20; fi
fi

# ------------------------------------------------------------------ run
RUN_ID="$(date -u +%Y-%m-%dT%H-%M-%SZ)-$TIER"
[ -z "$OUT" ] && OUT="$RUNS_DIR/$RUN_ID"
mkdir -p "$OUT"

precheck > "$OUT/precheck.txt" 2>&1 || { log "precheck failed"; cat "$OUT/precheck.txt" >&2; exit 20; }

CPID="$("$R" 'pgrep -f vmc-thinclient-app | head -1' 2>/dev/null | tr -d '[:space:]')"
log "client pid=$CPID  tier=$TIER  window=${WIN}s"

log "warm-up ${WARMUP_S}s (discarded: FIFO prefill, first resync)"
sleep "$WARMUP_S"

# Byte offsets mark the window edges; everything before t0 is warm-up.
C_START="$("$R" "stat -c %s $CLIENT_TLM" | tr -d '[:space:]')"
S_START="$(stat -c %s "$SERVER_TLM" 2>/dev/null || echo 0)"
T0="$(date +%s.%N)"

: > "$OUT/resources.ndjson"
: > "$OUT/heartbeat.log"

log "measuring ${WIN}s  (~$((WIN / 60)) one-minute buckets)"

# ---- in-run monitoring -----------------------------------------------------
# A doomed 21-minute run is aborted at ~90s of silence rather than waited out.
# Sampling is low-frequency and deliberate; the test agent itself stays idle.
stalls=0; last_bytes="$C_START"; aborted=0; elapsed=0
next_res=0
while [ "$elapsed" -lt "$WIN" ]; do
  sleep "$HEARTBEAT_S"
  elapsed=$(( $(date +%s) - ${T0%.*} ))
  now_bytes="$("$R" "stat -c %s $CLIENT_TLM 2>/dev/null || echo 0" | tr -d '[:space:]')"
  alive="$("$R" "kill -0 $CPID 2>/dev/null && echo 1 || echo 0" | tr -d '[:space:]')"
  printf '%s elapsed=%s bytes=%s alive=%s\n' "$(date -u +%H:%M:%S)" "$elapsed" "$now_bytes" "$alive" \
    >> "$OUT/heartbeat.log"

  if [ "${alive:-0}" != "1" ]; then
    log "client process died at t+${elapsed}s - aborting run"
    aborted=1; break
  fi
  if [ "${now_bytes:-0}" -le "${last_bytes:-0}" ]; then
    stalls=$((stalls + 1))
    log "telemetry stalled ($stalls/$HEARTBEAT_STALL_LIMIT) at t+${elapsed}s"
    if [ "$stalls" -ge "$HEARTBEAT_STALL_LIMIT" ]; then
      log "telemetry dead for $((stalls * HEARTBEAT_S))s - aborting run"
      aborted=1; break
    fi
  else
    stalls=0
  fi
  last_bytes="$now_bytes"

  # resource sample (leak detection across the long stretch)
  if [ "$elapsed" -ge "$next_res" ]; then
    next_res=$((next_res + RESSAMPLE_S))
    stats="$("$R" "rss=\$(awk '/VmRSS/{print \$2}' /proc/$CPID/status 2>/dev/null); \
                   fds=\$(ls /proc/$CPID/fd 2>/dev/null | wc -l); \
                   echo \"\$rss \$fds\"" 2>/dev/null)"
    rss="$(echo "$stats" | awk '{print $1}')"; fds="$(echo "$stats" | awk '{print $2}')"
    disk="$(du -sm "$DASH_OUT" 2>/dev/null | awk '{print $1}')"
    printf '{"t":"res","elapsed_s":%d,"rss_kb":%s,"fds":%s,"disk_mb":%s}\n' \
      "$elapsed" "${rss:-null}" "${fds:-null}" "${disk:-null}" >> "$OUT/resources.ndjson"
  fi
done

C_END="$("$R" "stat -c %s $CLIENT_TLM" | tr -d '[:space:]')"
S_END="$(stat -c %s "$SERVER_TLM" 2>/dev/null || echo 0)"
T1="$(date +%s.%N)"
ACTUAL="$(awk -v a="$T0" -v b="$T1" 'BEGIN{printf "%.2f", b-a}')"
log "window_actual_s=$ACTUAL  client_bytes=$((C_END - C_START))"

if [ "$((C_END - C_START))" -le 0 ]; then
  log "no client telemetry produced during the window"
  exit 22
fi

# ---- pull the sliced streams (gzip in transit: ~60 MB raw over Wi-Fi) ------
"$R" "tail -c +$((C_START + 1)) $CLIENT_TLM | head -c $((C_END - C_START)) | gzip -1" \
  2>/dev/null | gunzip > "$OUT/vmc_tlm_client.ndjson"
if [ "$((S_END - S_START))" -gt 0 ]; then
  tail -c +$((S_START + 1)) "$SERVER_TLM" | head -c $((S_END - S_START)) \
    > "$OUT/vmc_tlm_server.ndjson"
else
  : > "$OUT/vmc_tlm_server.ndjson"
fi

"$R" "tail -n 20000 $CLIENT_LOG" > "$OUT/dash_client.log" 2>/dev/null || true
tail -n 20000 "$SERVER_LOG" > "$OUT/dash_server.log" 2>/dev/null || true
"$R" "cat $ALSA_PCM_STATUS 2>/dev/null" > "$OUT/alsa_status.txt" 2>/dev/null || true
curl -s --max-time 3 "$MPD_URL" > "$OUT/live.mpd" 2>/dev/null || true

cat > "$OUT/meta.json" <<EOF
{
  "run_id": "$RUN_ID",
  "tier": "$TIER",
  "window_actual_s": $ACTUAL,
  "warmup_s": $WARMUP_S,
  "window_s": $WIN,
  "tail_exclude_s": $TAIL_EXCLUDE_S,
  "bucket_s": $BUCKET_S,
  "content_fps": $CONTENT_FPS,
  "audio_rate": $AUDIO_RATE,
  "audio_period": $AUDIO_PERIOD,
  "audio_fifo_bytes": $AUDIO_FIFO_BYTES,
  "seg_duration_s": $SEG_DURATION_S,
  "clip_duration_s": $CLIP_DURATION_S,
  "play_once": $PLAY_ONCE,
  "aborted_early": $aborted,
  "commit": "$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)",
  "server_ip": "$SERVER_IP",
  "client_ip": "$CLIENT_IP"
}
EOF

if [ "$aborted" = "1" ]; then
  log "run aborted early at t+${elapsed}s of ${WIN}s"
  echo "$OUT"; exit 23
fi

# A window off by more than 5s is not comparable across iterations. (5s, not
# 2s: a 21-minute run accumulates more scheduling slack than a 60s one.)
awk -v a="$ACTUAL" -v w="$WIN" 'BEGIN{ d=a-w; if (d<0) d=-d; exit (d>5.0)?1:0 }' || {
  log "window truncated/stretched: actual=$ACTUAL expected=$WIN"
  echo "$OUT"; exit 21;
}

echo "$OUT"
exit 0
