#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests the install step: package/launch.sh hands a staged update to
# package/update.sh, which swaps the app folder, starts the new version and
# restores the old one if the new one does not draw a frame. The programs
# are stubs; everything happens in a temporary folder.
#
#   sh tests/update-apply.sh PACKAGE_DIR
#
# POSIX sh: the device runs these scripts in BusyBox ash. Run by
# tests/update-e2e.sh; also safe to run on the device (it uses its own
# Wi-Fi fix session paths and touches nothing of the installed app).
set -u
pkg=$(cd "${1:?usage: update-apply.sh PACKAGE_DIR}" && pwd)
t=$(mktemp -d /tmp/update-apply.XXXXXX)
sd=$t/sd
app=$sd/Apps/MoonlightTSP
stage=$sd/.moonlight-tsp-update
ev=$t/events
MLTSP_DATA=$t/data
MOONLIGHT_OBSS_FLAG=$t/obss-flag
MOONLIGHT_OBSS_FREQUENCY_FILE=$t/obss-frequency
MOONLIGHT_OBSS_SESSIONS=$t/obss-sessions
export MLTSP_DATA MOONLIGHT_OBSS_FLAG MOONLIGHT_OBSS_FREQUENCY_FILE \
    MOONLIGHT_OBSS_SESSIONS
fails=0
n=0
trap 'rm -rf "$t"' EXIT

ok() { n=$((n + 1)); echo "ok   $1"; }
bad() { n=$((n + 1)); fails=$((fails + 1)); echo "FAIL $1"; }
is() {
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: \"$2\", expected \"$3\""; fi
}

# mkapp DIR VERSION UI: UI is how the stub UI behaves:
#   old      stages $t/incoming (as update-fetch does) and exits 3;
#            exits 0 if there is nothing to stage
#   three    always exits 3
#   good     writes $MLTSP_STARTED_FILE, exits 0
#   broken   exits 1 at once
#   late     writes $MLTSP_STARTED_FILE, then exits 1
mkapp() {
    mkdir -p "$1"
    cp "$pkg/launch.sh" "$pkg/update.sh" "$1/"
    echo "$2" >"$1/VERSION"
    echo '{}' >"$1/config.json"
    cat >"$1/moonlight-tsp-stream" <<EOF
#!/bin/sh
[ "\$1" = --version ] && { echo "moonlight-tsp-stream $2"; exit 0; }
echo "\$1 $2" >>"$ev"
EOF
    cat >"$1/moonlight-tsp-ui" <<EOF
#!/bin/sh
[ "\$1" = --version ] && { echo "moonlight-tsp-ui $2"; exit 0; }
echo "ui $2 \${MLTSP_STARTED_FILE:+started-file}" >>"$ev"
case $3 in
old)
    [ -e "$t/incoming/ready" ] || exit 0
    mkdir -p "\$MLTSP_STAGE"
    [ -d "$t/incoming/new" ] && mv "$t/incoming/new" "\$MLTSP_STAGE/new"
    mv "$t/incoming/ready" "\$MLTSP_STAGE/ready"
    exit 3 ;;
three) exit 3 ;;
good) : >"\$MLTSP_STARTED_FILE"; exit 0 ;;
broken) exit 1 ;;
late) : >"\$MLTSP_STARTED_FILE"; exit 1 ;;
esac
EOF
    chmod 755 "$1"/*
}
# setup NEW_UI [OLD_UI]: version 0.3.0 installed; its UI stages 0.3.1.
setup() {
    rm -rf "$sd" "$MLTSP_DATA" "$ev" "$t/incoming"
    mkapp "$app" 0.3.0 "${2:-old}"
    mkapp "$t/incoming/new/Apps/MoonlightTSP" 0.3.1 "$1"
    echo "0.3.1 0.3.0" >"$t/incoming/ready"
}
launch() {
    sh "$app/launch.sh"
    rc=$?
    events=$(grep '^ui' "$ev" | tr '\n' ',')
    result=$(cat "$MLTSP_DATA/update-result" 2>/dev/null)
}
common() {
    is "$1: no installer copy left" "$(ls /tmp/moonlight-tsp-update.* 2>/dev/null)" ""
    is "$1: Wi-Fi fix session ended" \
        "$(ls -d "$MOONLIGHT_OBSS_SESSIONS" "$MOONLIGHT_OBSS_FLAG" 2>/dev/null)" ""
    is "$1: display reset by each launcher" \
        "$(grep -c '^reset-display' "$ev")" "$2"
}

setup good
launch
is "update: exit status" "$rc" 0
is "update: programs" "$events" "ui 0.3.0 ,ui 0.3.1 started-file,"
is "update: installed" "$(cat "$app/VERSION")" 0.3.1
is "update: result" "$result" "updated 0.3.0 0.3.1"
is "update: stage removed" "$(ls -A "$sd" | tr '\n' ' ')" "Apps "
common update 2
grep -q "installed 0.3.1" "$MLTSP_DATA/logs/install.log" &&
    ok "update: install.log" || bad "update: install.log"

setup broken
launch
is "rollback: exit status" "$rc" 0
is "rollback: programs" "$events" "ui 0.3.0 ,ui 0.3.1 started-file,ui 0.3.0 ,"
is "rollback: old version back" "$(cat "$app/VERSION")" 0.3.0
is "rollback: result" "$result" "rolled-back 0.3.1 0.3.0"
is "rollback: logged" "$(grep -c 'restored 0.3.0; starting it' "$MLTSP_DATA/logs/install.log")" 1
# The restored launcher removes the stage with the failed version.
is "rollback: stage removed" "$(ls -d "$stage" 2>/dev/null)" ""
common rollback 3

setup late
launch
is "exit after the first frame: status" "$rc" 1
is "exit after the first frame: kept" "$(cat "$app/VERSION")" 0.3.1
is "exit after the first frame: result" "$result" "updated 0.3.0 0.3.1"
is "exit after the first frame: stage removed" "$(ls -d "$stage" 2>/dev/null)" ""

setup good three
launch
is "status 3 without ready: passed on" "$rc" 3
is "status 3 without ready: unchanged" "$(cat "$app/VERSION")" 0.3.0
is "status 3 without ready: no result" "$result" ""

setup good
rm -rf "$t/incoming/new"
launch
is "ready without new: old launcher again" "$events" "ui 0.3.0 ,ui 0.3.0 ,"
is "ready without new: unchanged" "$(cat "$app/VERSION")" 0.3.0
is "ready without new: ready removed" "$(ls "$stage" 2>/dev/null)" ""

# What a power-off during an update leaves, at the next normal start.
setup good
rm -rf "$t/incoming"
mkapp "$stage/previous" 0.2.0 old
: >"$stage/started"
launch
is "old stage after a power-off: removed" "$(ls -d "$stage" 2>/dev/null)" ""
setup good
rm -rf "$t/incoming"
mkapp "$stage/new/Apps/MoonlightTSP" 0.3.1 good
echo "0.3.1 0.3.0" >"$stage/ready"
launch
is "staged but not installed: removed" "$(ls -d "$stage" 2>/dev/null)" ""
is "staged but not installed: unchanged" "$(cat "$app/VERSION")" 0.3.0
setup good
rm -rf "$t/incoming"
mkapp "$stage/previous" 0.2.0 old
launch
is "previous that never started: kept" "$(ls "$stage" 2>/dev/null)" previous

echo "update-apply: $n checks, $fails failed"
[ "$fails" = 0 ]
