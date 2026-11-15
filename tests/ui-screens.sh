#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Walks the UI through its screens in the build container (SDL dummy video,
# fake engine) and saves BMP screenshots. Run from scripts/ui-screens.sh.
set -eu
ui=${UI:-/out/moonlight-tsp-ui-host}
shots=${SHOTS:-/out/shots}
export MLTSP_DATA=/tmp/ui-data FAKE_STATE=/tmp/fake-state
export SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software
rm -rf "$MLTSP_DATA" "$FAKE_STATE" "$shots"
mkdir -p "$MLTSP_DATA" "$shots"
fifo=/tmp/ui.fifo
rm -f "$fifo"

hosts() {
    cat > "$MLTSP_DATA/config.ini" <<EOF
fps=60
bitrate=10000
codec=auto
last_host=0
host=10.0.0.1 GAMING-PC
host=10.0.0.2 Living room
host=10.0.0.3 Laptop
EOF
}
start() {
    case $1 in first*) ;; *) hosts ;; esac
    "$ui" --engine /src/tests/fake-engine.sh --debug-fifo "$fifo" \
        2> "$shots/ui-$1.log" &
    pid=$!
    for _ in $(seq 50); do [ -p "$fifo" ] && break; sleep 0.1; done
    sleep 0.5
}
# A write never blocks the run if the UI has exited.
k() { timeout 2 sh -c 'echo "$1" > "$2"' - "$1" "$fifo" || echo "ui gone at: $1"; sleep "${2:-0.3}"; }
shot() { k "shot $shots/$1.bmp" 0.4; }
finish() {
    k quit 0.5
    timeout 10 sh -c "while kill -0 $pid 2>/dev/null; do sleep 0.2; done" ||
        { echo "ui did not exit; killing"; kill -9 "$pid"; }
    wait "$pid" || echo "ui exit status $?"
    rm -f "$fifo"
}

# First start: no hosts, opens the search.
start first
sleep 4.5
shot 01-search-empty
k b
shot 02-hosts-empty
k down; k a
shot 03-addip
k a; k a; k right; k a
shot 03b-addip-typed
finish

# Saved hosts: one online, one unreachable, one unpaired.
mkdir -p "$FAKE_STATE"
echo 2 > "$FAKE_STATE/running"
start hosts
shot 04-hosts-checking
sleep 5
shot 05-hosts-status
k a 1.2
shot 06-apps
k down; k down; k a
shot 07-switch-app-dialog
k b
k x
shot 08-settings
k down; k right; k down; k down; k down; k down
shot 09-settings-changed
k b
k up; k up; k a 1.2
shot 10-connecting
sleep 2
shot 11-streaming
finish

# Stream menu and disconnect, then pairing the unpaired host.
FAKE_MENU=1 start menu
sleep 1.5
k a 1.2
k a 2.2
sleep 2.2
shot 12-stream-menu
# Mouse mode: toggled on, back to the stream, shown as on next time.
k down; k a 0.3
grep -q "^mouse on$" "$FAKE_STATE/ipc.log" || echo "FAIL: no mouse on sent"
grep -A1 "^mouse on$" "$FAKE_STATE/ipc.log" | grep -q "^resume$" ||
    echo "FAIL: mouse on not followed by resume"
sleep 3.2
shot 12b-stream-menu-mouse
k down; k down; k down; k a 0.2
shot 13-disconnecting
sleep 1.5
shot 14-apps-after
k b
k down; k down; k a 0.8
shot 15-pair-pin
sleep 3.5
shot 16-paired-apps
finish

# The connection drops during the stream.
FAKE_TERM=1 start term
sleep 1.5
k a 1.2
k a 7
shot 17-lost-connection
finish

# The engine stops sending heartbeats: the UI records its threads and
# kills it.
FAKE_HANG=1 start hang
sleep 1.5
k a 1.2
k a 13
shot 25-engine-hung
grep -q '^thread ' "$MLTSP_DATA/logs/hang.log" &&
    echo "hang.log: $(grep -c '^thread ' "$MLTSP_DATA/logs/hang.log") threads" ||
    echo "FAIL: no thread dump in hang.log"
grep -q '^fds:' "$MLTSP_DATA/logs/hang.log" ||
    echo "FAIL: no descriptor table in hang.log"
grep -h "heartbeat lost" "$shots/ui-hang.log" | sed 's/^/ui.log: /'
finish

# Heartbeats that arrive in a later millisecond than the UI's clock
# reading must not look 2^64 ms old (the watchdog once killed healthy
# streams that way).
MLTSP_TEST_DRAIN_DELAY_US=2000 start late-lines
sleep 1.5
k a 1.2
k a 5
if grep -q "heartbeat lost" "$shots/ui-late-lines.log"; then
    echo "FAIL: watchdog fired on heartbeats read late"
    grep -h "heartbeat lost" "$shots/ui-late-lines.log"
else
    echo "late lines: no watchdog kill"
fi
finish

# A connection stage fails; removing a host; an unreachable host.
FAKE_STAGE_FAIL=1 start stage
sleep 1.5
k a 1.2
k a 3
shot 18-stage-failed
k a
k b
k down; k y
shot 19-remove-dialog
k b
k a 2.2
shot 20-unreachable
finish

# The CrossMix CPU Performance scene is detected from its command line.
printf 'while :; do sleep 1; done\n' > /tmp/com.trimui.cpuperformance.sh
sh /tmp/com.trimui.cpuperformance.sh 1 &
scene=$!
start perf
sleep 1
shot 21-perf-scene-warning
finish
kill "$scene"

# Search results are probed, so already-paired PCs stand out; adding one
# keeps its status instead of checking again.
rm -f "$MLTSP_DATA/config.ini" "$FAKE_STATE"/paired-*
start first-search
sleep 4.5
k "mdns 10.0.0.1 GAMING-PC" 0.05
k "mdns 10.0.0.2 Living room" 0.05
k "mdns 10.0.0.3 Laptop" 0.05
shot 22-search-checking
sleep 3
shot 23-search-status
k a 0.2
shot 24-hosts-added
finish
echo "screens: $(ls "$shots"/*.bmp | wc -l)"
