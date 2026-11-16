#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Moonlight HW (moonlight-tsp) launcher for CrossMix-OS.
#
# - Never touches the CPU governor or frequency limits.
# - Joins the Moonlight Wi-Fi (OBSS) fix's session contract exactly like
#   that fix's launcher wrapper: a per-launch token, the activation flag and
#   the current 2.4 GHz frequency; the last session out removes the flag.
# - Always runs "moonlight-tsp-stream reset-display" afterwards, so a crashed
#   or killed UI cannot leave a video layer over MainUI or Wi-Fi power save
#   switched off.
# - If the UI exits with status 3, it has staged an update: update.sh
#   installs it and starts the new version (see that script).
progdir=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$progdir" || exit 1
# The update stage is on the same SD card as the app, so installing an
# update is two renames.
MLTSP_STAGE=$(cd "$progdir/../.." && pwd)/.moonlight-tsp-update
export MLTSP_STAGE

data=${MLTSP_DATA:-/mnt/UDISK/trimui-tools/moonlight-tsp}
export MLTSP_DATA="$data"
logs="$data/logs"
mkdir -p "$logs"
log="$logs/launcher.log"
# Keep the previous run's logs for comparison.
[ -f "$log" ] && mv -f "$log" "$log.1"
[ -f "$logs/ui.log" ] && mv -f "$logs/ui.log" "$logs/ui.log.1"
[ -f "$logs/update.log" ] && mv -f "$logs/update.log" "$logs/update.log.1"

# update.sh removes the stage when it ends, but a power-off can stop it
# first. Then remove the stage now, unless it holds a previous version
# that never showed a frame (README: rename it back by hand).
if [ -z "${MLTSP_STARTED_FILE:-}" ] && [ -d "$MLTSP_STAGE" ] &&
    { [ ! -d "$MLTSP_STAGE/previous" ] || [ -e "$MLTSP_STAGE/started" ]; }
then
    echo "$(date '+%F %T') launcher: removing an old $MLTSP_STAGE" >>"$log"
    rm -rf "$MLTSP_STAGE"
fi

flag=${MOONLIGHT_OBSS_FLAG:-/tmp/moonlight-no-obss-scan}
frequency_file=${MOONLIGHT_OBSS_FREQUENCY_FILE:-/tmp/moonlight-obss-current-frequency}
sessions=${MOONLIGHT_OBSS_SESSIONS:-/tmp/moonlight-obss-sessions}
token="$sessions/$$"
stay_awake_owned=0

cleanup()
{
    "$progdir/moonlight-tsp-stream" reset-display >>"$log" 2>&1
    rm -f "$token"
    if rmdir "$sessions" 2>/dev/null; then
        rm -f "$flag" "$frequency_file"
    fi
    if [ "$stay_awake_owned" = 1 ]; then
        rm -f /tmp/stay_awake
    fi
    echo "$(date '+%F %T') launcher: done" >>"$log"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

echo "$(date '+%F %T') launcher: start (pid $$)" >>"$log"

# The device must not auto-sleep during a stream (as the stock client).
if [ ! -e /tmp/stay_awake ]; then
    echo 1 >/tmp/stay_awake && stay_awake_owned=1
fi

mkdir -p "$sessions"
touch "$token" "$flag"
current_frequency=$(iw dev wlan0 link 2>/dev/null |
    sed -n 's/^[[:space:]]*freq: \([0-9][0-9]*\).*$/\1/p')
case "$current_frequency" in
    2[4][0-9][0-9])
        printf '%s\n' "$current_frequency" >"$frequency_file"
        echo "launcher: Wi-Fi fix session on $current_frequency MHz" >>"$log"
        ;;
    *)
        echo "launcher: no 2.4 GHz frequency (\"$current_frequency\")" >>"$log"
        ;;
esac

# SDL2 and SDL2_ttf with the Mali fbdev driver live here.
export LD_LIBRARY_PATH="/usr/trimui/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$progdir/moonlight-tsp-ui" --engine "$progdir/moonlight-tsp-stream" \
    2>"$logs/ui.log"
rc=$?
echo "$(date '+%F %T') launcher: ui exited with $rc" >>"$log"
if [ "$rc" = 3 ] && [ -f "$MLTSP_STAGE/ready" ] && [ -f "$progdir/update.sh" ]
then
    # Install the staged update. update.sh moves this folder, so it runs
    # from a copy in /tmp, after this launcher's own clean-up.
    cleanup
    trap - EXIT HUP INT TERM
    installer=/tmp/moonlight-tsp-update.$$.sh
    if cp "$progdir/update.sh" "$installer"; then
        exec sh "$installer" apply "$progdir" "$MLTSP_STAGE" "$data"
    fi
    echo "$(date '+%F %T') launcher: cannot copy update.sh" >>"$log"
    exit 1
fi
exit "$rc"
