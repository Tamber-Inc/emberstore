#!/usr/bin/env bash
# Builds bytelayout, regenerates the byte annotations and serves the page on
# macprorack's Tailscale address, in a detached tmux session named bytelayout.
#
#   Apps/ByteLayout/serve.sh            build, generate, (re)start the server
#   Apps/ByteLayout/serve.sh --foreground   same, but serve in this terminal
#
# BYTELAYOUT_ADDRESS and BYTELAYOUT_PORT override where it listens.
set -euo pipefail

appDirectory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repoDirectory="$(cd "$appDirectory/../.." && pwd)"
buildDirectory="$repoDirectory/build"
siteDirectory="$buildDirectory/Apps/ByteLayout/site"
listenAddress="${BYTELAYOUT_ADDRESS:-100.104.147.35}"
listenPort="${BYTELAYOUT_PORT:-8742}"
sessionName="bytelayout"

if [[ ! -f "$buildDirectory/CMakeCache.txt" ]]; then
    cmake -B "$buildDirectory" -S "$repoDirectory" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DEACP_BUILD_GRAPHICS=OFF
fi

cmake --build "$buildDirectory" --target bytelayout

mkdir -p "$siteDirectory"
cp "$appDirectory"/web/* "$siteDirectory/"
"$buildDirectory/Apps/ByteLayout/bytelayout" "$siteDirectory/data.js"

serveCommand=(python3 -m http.server "$listenPort" --bind "$listenAddress" --directory "$siteDirectory")

if [[ "${1:-}" == "--foreground" ]]; then
    exec "${serveCommand[@]}"
fi

command tmux kill-session -t "$sessionName" 2>/dev/null || true
command tmux new-session -d -s "$sessionName" "${serveCommand[*]}"
echo "serving http://$listenAddress:$listenPort/ (tmux session $sessionName)"
