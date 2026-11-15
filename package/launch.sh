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
progdir=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$progdir" || exit 1

data=${MLTSP_DATA:-/mnt/UDISK/trimui-tools/moonlight-tsp}
logs="$data/logs"
mkdir -p "$logs"
log="$logs/launcher.log"
# Keep the previous run's logs for comparison.
[ -f "$log" ] && mv -f "$log" "$log.1"
[ -f "$logs/ui.log" ] && mv -f "$logs/ui.log" "$logs/ui.log.1"

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
exit "$rc"
