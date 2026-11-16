#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stands in for moonlight-tsp-stream in UI tests: speaks the IPC protocol on
# the --ipc-fd socket. Host 10.0.0.2 is unreachable, 10.0.0.3 unpaired.
# FAKE_STATE (a directory) records pairing and the running app.
# FAKE_UPDATE: update-check finds nothing new (unset), "available" or
# "error"; FAKE_FETCH=fail makes update-fetch fail after some progress.
state=${FAKE_STATE:-/tmp/fake-engine}
mkdir -p "$state"
host="" ipc=3 cmd="" arg=""
while [ $# -gt 0 ]; do
    case $1 in
    --host) host=$2; shift 2 ;;
    --keydir|--fps|--bitrate|--codec|--port|--stage|--update-url|\
    --ca-file|--unzip) shift 2 ;;
    --ipc-fd) ipc=$2; shift 2 ;;
    --*) shift ;;
    *) if [ -z "$cmd" ]; then cmd=$1; else arg=$1; fi; shift ;;
    esac
done
echo "fake-engine: $cmd $arg (host $host)"
say() { echo "$*" >&"$ipc"; }
running=$(cat "$state/running" 2>/dev/null || echo 0)
paired=1
[ "$host" = 10.0.0.3 ] && [ ! -e "$state/paired-$host" ] && paired=0

case $cmd in
reset-display) exit 0 ;;
esac
sleep 0.3
case $cmd in
update-check)
    echo "update-check" >> "$state/updates.log"
    sleep "${FAKE_UPDATE_DELAY:-0}"
    case ${FAKE_UPDATE:-} in
    available) say "update available 0.3.1 4312345 Faster pairing and a new stream menu" ;;
    error) say "update error The connection to GitHub failed. Make sure that Wi-Fi is on."
           exit 1 ;;
    *) say "update none 0.3.0" ;;
    esac
    exit 0 ;;
update-fetch)
    echo "update-fetch $arg" >> "$state/updates.log"
    total=4312345
    for got in 0 524288 1310720 2097152 2883584; do
        say "update progress $got $total"; sleep 0.5
    done
    if [ "${FAKE_FETCH:-}" = fail ]; then
        say "update error The download is damaged. Try again."
        exit 1
    fi
    say "update progress $total $total"
    say "update verify"; sleep 1
    say "update staged $arg"
    exit 0 ;;
esac
if [ "$host" = 10.0.0.2 ]; then
    sleep 1
    say "error unreachable"
    exit 1
fi
case $cmd in
info)
    say "host $paired $running 1" ;;
list)
    [ $paired = 1 ] || { say "error not-paired"; exit 1; }
    for a in "1 Desktop" "2 Steam Big Picture" "3 Cyberpunk 2077" \
             "4 Hades II" "5 Elden Ring Nightreign" "6 Hollow Knight: Silksong" \
             "7 Balatro" "8 Celeste" "9 Stardew Valley"; do
        id=${a%% *}
        r=0; [ "$id" = "$running" ] && r=1
        say "app $id $r ${a#* }"
    done ;;
pair)
    say "pin 4821"
    sleep "${FAKE_PAIR_DELAY:-3}"
    touch "$state/paired-$host"
    say "paired" ;;
quit)
    echo 0 > "$state/running"
    say "quit-done" ;;
stream)
    if [ "$running" != 0 ] && [ "$running" != "$arg" ]; then
        say "error other-app-running $running"; exit 1
    fi
    for s in "platform initialization" "name resolution" "RTSP handshake" \
             "control stream initialization" "video stream initialization"; do
        say "stage $s"; sleep 0.4
    done
    if [ -n "${FAKE_STAGE_FAIL:-}" ]; then
        say "error stage-failed RTSP handshake -1"
        say "error connect-failed -1"
        exit 1
    fi
    echo "$arg" > "$state/running"
    say "started"
    t=0
    while :; do
        if read -r -t 1 line <&"$ipc"; then
            echo "$line" >> "$state/ipc.log"
            case $line in
            mouse*) menu_at=$((t + 2)) ;;
            stop) say "ended"; exit 0 ;;
            quit-app) echo 0 > "$state/running"; say "ended"; exit 0 ;;
            esac
            continue
        fi
        t=$((t + 1))
        # A hung engine: alive but silent after 2 s.
        [ -n "${FAKE_HANG:-}" ] && [ $t -gt 2 ] && continue
        say "stats fps=60 lat=9.8 dec=2.31 rtt=4 kbps=9850 lost=0 drop=1 cpu=6.2"
        say "hb $((t * 60))"
        # A menu press after 2 s (the engine reports it in real use).
        # A menu press after 2 s, and 2 s after a mouse mode change (the
        # engine reports them in real use).
        { [ $t = 2 ] || [ "$t" = "${menu_at:-}" ]; } &&
            [ -n "${FAKE_MENU:-}" ] && say "menu"
        if [ -n "${FAKE_TERM:-}" ] && [ $t = 3 ]; then
            say "term -1 This device lost the connection to the PC."
            say "ended"
            exit 0
        fi
    done ;;
*) exit 2 ;;
esac
exit 0
