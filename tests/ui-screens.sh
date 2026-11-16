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
# A write never blocks the run if the UI has exited.
k() { timeout 2 sh -c 'echo "$1" > "$2"' - "$1" "$fifo" || echo "ui gone at: $1"; sleep "${2:-0.3}"; }
# KEEP=1 keeps the config of the previous run. The container has no
# battery or Wi-Fi: the header shows STATUS (default 78%, -55 dBm).
start() {
    case $1 in first*) ;; *) [ -n "${KEEP:-}" ] || hosts ;; esac
    "$ui" --engine /src/tests/fake-engine.sh --debug-fifo "$fifo" \
        2> "$shots/ui-$1.log" &
    pid=$!
    for _ in $(seq 50); do [ -p "$fifo" ] && break; sleep 0.1; done
    k "status ${STATUS:-78 discharging -55}" 0.5
}
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

# Battery and Wi-Fi in the header: low battery and a weak signal, then
# charging without Wi-Fi, then a device without a battery.
STATUS="9 discharging -78" start status-low
sleep 1
shot 37-status-low
k "status 54 charging off"
shot 38-status-charging-no-wifi
k a 1.2
k "status none discharging -66"
shot 39-status-no-battery
finish
grep -q "status: battery 54% charging, wifi not connected" \
    "$shots/ui-status-low.log" && echo "status: logged" ||
    echo "FAIL: no status line in the log"

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
# ---- updates ----
# Waits for the UI to exit by itself and checks its exit status.
expect_exit() {
    local rc=0
    timeout 15 sh -c "while kill -0 $pid 2>/dev/null; do sleep 0.2; done" ||
        { echo "FAIL: ui did not exit"; kill -9 "$pid"; }
    wait "$pid" || rc=$?
    [ "$rc" = "$1" ] && echo "ui exit status $rc (expected)" ||
        echo "FAIL: ui exit status $rc, expected $1"
    rm -f "$fifo"
}
cfg() { grep -q "^$1\$" "$MLTSP_DATA/config.ini" && echo "config: $1" ||
        echo "FAIL: config has no \"$1\""; }
# Settings, then the row "Check for updates now" (the last of nine).
settings_update_now() {
    k x
    for _ in 1 2 3 4 5 6 7 8; do k down 0.1; done
}

# Nothing new: the start-up check runs once, quietly. MLTSP_STARTED_FILE
# is written after the first frame (update.sh uses it).
rm -f "$FAKE_STATE/updates.log" /tmp/ui-started
FAKE_UPDATE_DELAY=2 MLTSP_STARTED_FILE=/tmp/ui-started start update-none
sleep 5
[ -e /tmp/ui-started ] && echo "started file written" ||
    echo "FAIL: no started file"
[ "$(grep -c '^update-check$' "$FAKE_STATE/updates.log" 2>/dev/null)" = 1 ] &&
    echo "start-up check: once" || echo "FAIL: start-up check count"
k x
for _ in 1 2 3 4 5 6 7; do k down 0.1; done
shot 26-settings-update-check
k down
shot 27-settings-update-now
k a 0.5
shot 28-update-checking
sleep 2.2
shot 29-update-newest
k a
finish

# A new version at start: the dialog, with Later focused.
FAKE_UPDATE=available start update-offer
sleep 3.5
shot 30-update-available
k a
# Check for updates now offers it again; Skip stores the version.
settings_update_now
k a 1
k right; k a
finish
cfg update_skip=0.9.0

# A skipped version is not offered at start.
KEEP=1 FAKE_UPDATE=available start update-skipped
sleep 3.5
grep -q "0.9.0 was skipped before" "$shots/ui-update-skipped.log" &&
    echo "skipped version: no dialog" || echo "FAIL: skipped version offered"
finish

# Update: progress, then exit status 3 for launch.sh to install it.
rm -f "$FAKE_STATE/updates.log"
FAKE_UPDATE=available start update-fetch
sleep 3.5
k left; k a 1.3
shot 31-update-downloading
grep -q "^update-fetch 0.9.0$" "$FAKE_STATE/updates.log" &&
    echo "fetch: 0.9.0" || echo "FAIL: no update-fetch 0.9.0"
expect_exit 3

# B stops a download.
FAKE_UPDATE=available start update-stop
sleep 3.5
k left; k a 1
k b 0.5
shot 32-update-stopped
finish

# A damaged download.
FAKE_UPDATE=available FAKE_FETCH=fail start update-fail
sleep 3.5
k left; k a 3.5
shot 33-update-failed
finish

# A failed check from Settings.
FAKE_UPDATE=error start update-error
sleep 3.5
settings_update_now
k a 1
shot 34-update-check-error
finish

# Check at start Off: no check at all.
start update-off
k x
for _ in 1 2 3 4 5 6 7; do k down 0.1; done
k right; k b
finish
cfg update_check=0
rm -f "$FAKE_STATE/updates.log"
KEEP=1 FAKE_UPDATE=available start update-off2
sleep 4
finish
[ ! -s "$FAKE_STATE/updates.log" ] && echo "check off: no check" ||
    echo "FAIL: checked with the setting off"

# What update.sh reports after a restart, shown once.
echo "updated 0.2.0 0.3.0" > "$MLTSP_DATA/update-result"
start update-installed
shot 35-update-installed
[ ! -e "$MLTSP_DATA/update-result" ] && echo "update-result removed" ||
    echo "FAIL: update-result kept"
finish
echo "rolled-back 0.9.0 0.3.0" > "$MLTSP_DATA/update-result"
start update-rolled-back
shot 36-update-rolled-back
finish
cfg update_skip=0.9.0
echo "screens: $(ls "$shots"/*.bmp | wc -l)"
