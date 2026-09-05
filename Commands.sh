#!/bin/bash
# ============================================================
# Commands.sh — manual launch for the VMC LL-DASH live stream.
#
#   Server (MEC host) : 192.168.0.126  (swiftrade, RTX 5080, NVENC)
#   Thin client       : 192.168.0.145  (ai2, CUVID + DRM/HDMI + ALSA)
#
# Run the SERVER section on 192.168.0.126 and the CLIENT section on
# 192.168.0.145. Build each machine's binary locally (do NOT copy the
# host-built binary across — the client's FFmpeg ABI differs).
# ============================================================
set -u

INPUT=/home/deepak7121/FLUX3/1225am0807/swiftrade_av_60s_20260807_110902.mp4

# ============================================================
# 0. CLEANUP (run before starting, on each machine)
# ============================================================
cleanup() {
    echo "==> killing stale vmc processes"
    pkill -9 -x vmc-dash-sim 2>/dev/null
    pkill -9 -x vmc-thinclient-app 2>/dev/null
    pkill -9 -x vmc-mec-sim 2>/dev/null
    pkill -9 -x ffmpeg 2>/dev/null
    pkill -9 -f 'h264_nvenc' 2>/dev/null
    sleep 1
    rm -rf /tmp/vmc_dash_out
    echo "==> done"
}

# ============================================================
# 1. SERVER — MEC host 192.168.0.126
#    Build once: cmake -S . -B build-server-debug -DVMC_ENABLE_TESTS=OFF -DVMC_DEBUG=1
#                cmake --build build-server-debug --target vmc-dash-sim -j$(nproc)
# ============================================================
start_server() {
    # Use 0.0.0.0 so the thin client on 192.168.0.145 can reach us.
    setsid nohup ./build-server-debug/tools/vmc-dash-sim/vmc-dash-sim \
        --input "$INPUT" \
        --port 8080 --log-level 1 > /tmp/dash_server.log 2>&1 < /dev/null &
    echo "server pid $!"
    sleep 8
    # Verify: manifest reachable on the LAN + segments being produced
    curl -s -o /dev/null -w "MPD http %{http_code}\n" http://192.168.0.126:8080/live.mpd
    ss -tlnp | grep 8080
}

# ============================================================
# 2. CLIENT — thin client 192.168.0.145
#    Build once: cmake -S . -B build-debug -DVMC_ENABLE_TESTS=OFF -DVMC_DEBUG=1
#                cmake --build build-debug --target vmc-thinclient-app -j$(nproc)
#    One-time ALSA config for the HDMI monitor (card 1 / device 3 = NVIDIA HDA):
#      printf 'pcm.hdmi { type hw; card 1; device 3; }\npcm.!default { type hw; card 1; device 3; }\n' \
#        | sudo tee /etc/vmc-audio.conf
# ============================================================
start_client() {
    # VMC_DRM=1 for GPU scanout (Design B); omit for the /dev/fb0 path.
    # VMC_HUD=1 adds a green on-screen latency overlay.
    setsid nohup env VMC_DRM=1 VMC_HUD=1 \
        VMC_AUDIO_DEV=hdmi ALSA_CONFIG_PATH=/etc/vmc-audio.conf \
        ./build-debug/vmc-thinclient-app --dash http://192.168.0.126:8080/live.mpd 1 \
        > /tmp/dash_client.log 2>&1 < /dev/null &
    echo "client pid $!"
    sleep 10
    echo "==> logs:"
    grep -aE 'alsa:|display:|dash dbg|resync' /tmp/dash_client.log | tail -5
}

# ============================================================
# 3. STOP (run on each machine when done)
# ============================================================
stop() {
    pkill -9 -x vmc-dash-sim 2>/dev/null
    pkill -9 -x vmc-thinclient-app 2>/dev/null
    pkill -9 -x vmc-mec-sim 2>/dev/null
    pkill -9 -x ffmpeg 2>/dev/null
    sleep 1
    ss -tlnp | grep 8080 || echo "port 8080 free"
}

# ============================================================
# 4. HEALTH (on the client)
# ============================================================
health() {
    echo "==> client stats (last 3):"
    grep -aE 'dash stats:|dash dbg:' /tmp/dash_client.log | tail -3
    echo "==> audio:"
    cat /proc/asound/card1/pcm3p/sub0/status | grep state
    echo "  healthy looks like: drop=0 xrun(r/f)=0/0 and a bounded av-offset"
}

usage() {
    cat <<EOF
usage: $0 {cleanup|start_server|start_client|stop|health}
  start_server   on 192.168.0.126
  start_client   on 192.168.0.145
EOF
}

case "${1:-}" in
    cleanup)       cleanup ;;
    start_server)  start_server ;;
    start_client)  start_client ;;
    stop)          stop ;;
    health)        health ;;
    *)             usage ;;
esac
