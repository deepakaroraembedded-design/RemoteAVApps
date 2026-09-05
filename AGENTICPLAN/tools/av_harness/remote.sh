#!/usr/bin/env bash
# Run a command on the thin client (192.168.0.145) with a hard timeout.
# No interactive state is ever held on the client.
#
#   tools/av_harness/remote.sh 'pgrep -a vmc-thinclient-app'
#   tools/av_harness/remote.sh --quiet 'test -f /tmp/x'
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
. "$HERE/env.sh"

QUIET=0
if [ "${1:-}" = "--quiet" ]; then QUIET=1; shift; fi

if [ $# -lt 1 ]; then
  echo "usage: remote.sh [--quiet] <command...>" >&2
  exit 2
fi

SSH_OPTS=(
  -o BatchMode=yes
  -o StrictHostKeyChecking=accept-new
  -o ConnectTimeout=10
  -o ServerAliveInterval=5
  -o ServerAliveCountMax=3
)
[ -f "$CLIENT_SSH_KEY" ] && SSH_OPTS+=(-i "$CLIENT_SSH_KEY")

if [ "$QUIET" = "1" ]; then
  timeout "$SSH_TIMEOUT" ssh "${SSH_OPTS[@]}" "${CLIENT_USER}@${CLIENT_IP}" "$*" >/dev/null 2>&1
else
  timeout "$SSH_TIMEOUT" ssh "${SSH_OPTS[@]}" "${CLIENT_USER}@${CLIENT_IP}" "$*"
fi
rc=$?
if [ $rc -eq 124 ]; then
  echo "remote.sh: TIMEOUT after ${SSH_TIMEOUT}s: $*" >&2
fi
exit $rc
