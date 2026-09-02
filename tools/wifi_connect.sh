#!/bin/bash
# ============================================================
# wifi_connect.sh — connect the VMC thin client to a Wi-Fi AP.
#
# Supports WPA2/WPA3 (password) and open networks, brings the
# interface up via wpa_supplicant + dhclient, and verifies.
#
# Usage:
#   sudo ./wifi_connect.sh <ssid> [password] [interface]
#   sudo ./wifi_connect.sh                      # reads /etc/wifi_connect.conf
#
# Config file format (when no args given):
#   SSID=MyNetwork
#   PASSWORD=secret
#   INTERFACE=wlp6s0
# ============================================================

set -e

CONF_FILE="/etc/wifi_connect.conf"
IFACE="${3:-wlp6s0}"

if [ "$#" -ge 1 ]; then
    SSID="$1"
    PSK="${2:-}"
else
    if [ ! -f "$CONF_FILE" ]; then
        echo "ERROR: no args and $CONF_FILE not found." >&2
        echo "Usage: sudo $0 <ssid> [password] [interface]" >&2
        exit 1
    fi
    # shellcheck disable=SC1090
    . "$CONF_FILE"
    IFACE="${INTERFACE:-wlp6s0}"
fi

if [ -z "$SSID" ]; then
    echo "ERROR: SSID is empty." >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root (sudo)." >&2
    exit 1
fi

echo "============================================"
echo "  VMC thin client — Wi-Fi connect"
echo "  SSID:       $SSID"
echo "  Interface:  $IFACE"
echo "  Auth:       $([ -n "$PSK" ] && echo WPA2/WPA3 || echo OPEN)"
echo "============================================"

# --- 1. Dependencies -------------------------------------------------
echo "[1/5] Ensuring wpa_supplicant + dhclient..."
if ! command -v wpa_supplicant >/dev/null 2>&1 || ! command -v dhclient >/dev/null 2>&1; then
    apt-get update -y >/dev/null 2>&1 || true
    # NB: package is 'wpasupplicant' (no underscore) on Ubuntu 24.04+
    apt-get install -y wpasupplicant isc-dhcp-client >/dev/null
fi

# --- 2. Interface up ---------------------------------------------------
echo "[2/5] Bringing up $IFACE..."
ip link set "$IFACE" up || { echo "ERROR: cannot bring up $IFACE"; exit 1; }

# --- 3. wpa_supplicant config ------------------------------------------
echo "[3/5] Writing wpa_supplicant config..."
if [ -n "$PSK" ]; then
    wpa_passphrase "$SSID" "$PSK" > /etc/wpa_supplicant/wpa_supplicant.conf
else
    printf 'network={\n  ssid="%s"\n  key_mgmt=NONE\n}\n' "$SSID" \
        > /etc/wpa_supplicant/wpa_supplicant.conf
fi

# --- 4. Connect --------------------------------------------------------
echo "[4/5] Connecting via wpa_supplicant..."
pkill -x wpa_supplicant 2>/dev/null || true
pkill -x dhclient 2>/dev/null || true
sleep 1

wpa_supplicant -B -i "$IFACE" -c /etc/wpa_supplicant/wpa_supplicant.conf
dhclient "$IFACE"

# --- 5. Verify ----------------------------------------------------------
echo "[5/5] Verifying..."
sleep 2
LINK=$(iw dev "$IFACE" link 2>/dev/null)
if echo "$LINK" | grep -q "Connected to"; then
    echo ""
    echo "CONNECTED:"
    echo "$LINK" | grep -E "Connected to|SSID|signal|freq|tx bitrate" | sed 's/^/  /'
    ADDR=$(ip -4 -br addr show "$IFACE" 2>/dev/null | awk '{print $3}')
    echo "  IP:        ${ADDR:-none}"
    if [ -n "$ADDR" ]; then
        if ping -c 2 -W 2 8.8.8.8 >/dev/null 2>&1; then
            echo "  Internet:  OK"
        else
            echo "  Internet:  no route to 8.8.8.8 (AP may be offline)"
        fi
    fi
    exit 0
else
    echo ""
    echo "NOT CONNECTED. Link state:"
    echo "$LINK" | sed 's/^/  /'
    echo "Check: iw dev $IFACE scan | grep SSID"
    exit 1
fi
