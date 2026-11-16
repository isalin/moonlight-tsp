#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs an update that "moonlight-tsp-stream update-fetch" staged, then
# starts it. launch.sh runs a /tmp copy of the installed version's script
# when the UI exits with status 3:
#
#   update.sh apply APP_DIR STAGE DATA
#
# STAGE (named .moonlight-tsp-update, on the same SD card as APP_DIR) holds
#   ready                      "NEW OLD"
#   new/Apps/MoonlightTSP/     the checked new version
# and, while this script runs, previous/ (the old version) and started (the
# new UI writes it after its first frame, through MLTSP_STARTED_FILE).
#
# If the new version exits without drawing a frame, the old version comes
# back, data/update-result says "rolled-back NEW OLD" and the old UI shows
# that once (its launcher then removes STAGE with failed/, the new
# version). Otherwise it says "updated OLD NEW" and STAGE is removed.
#
# If the power goes off between the two renames below, APP_DIR is missing:
# rename STAGE/previous back to APP_DIR on a computer.
# POSIX sh only: this runs in BusyBox ash.

usage()
{
    echo "usage: update.sh apply APP_DIR STAGE DATA" >&2
    exit 2
}

[ "$#" = 4 ] && [ "$1" = apply ] || usage
app=$2
stage=$3
data=$4
case $app in /*) ;; *) usage ;; esac
case $stage in /*/.moonlight-tsp-update) ;; *) usage ;; esac

# The copy in /tmp is no longer needed once the shell has it open.
case $0 in /tmp/moonlight-tsp-update.*) rm -f "$0" ;; esac

mkdir -p "$data/logs"
log="$data/logs/install.log"
say()
{
    echo "$(date '+%F %T') update: $*" >>"$log"
}

new="$stage/new/Apps/MoonlightTSP"
prev="$stage/previous"
failed="$stage/failed"
started="$stage/started"
result="$data/update-result"
cd / || exit 1

to=
from=
read -r to from <"$stage/ready" 2>/dev/null
if [ -z "$to" ] || [ ! -f "$new/launch.sh" ] || [ ! -d "$app" ]; then
    say "nothing to install (ready \"$to $from\")"
    rm -f "$stage/ready"
    [ -f "$app/launch.sh" ] && exec sh "$app/launch.sh"
    exit 1
fi
say "installing $to over $from in $app"
rm -rf "$prev" "$failed"
rm -f "$started" "$stage/ready"

if ! mv "$app" "$prev"; then
    say "cannot move $app away; nothing changed"
    exec sh "$app/launch.sh"
fi
if ! mv "$new" "$app"; then
    say "cannot move the new version in; restoring $from"
    mv "$prev" "$app"
    sync
    exec sh "$app/launch.sh"
fi
rm -rf "$stage/new"
sync
printf 'updated %s %s\n' "$from" "$to" >"$result"
say "installed $to; starting it"

MLTSP_STARTED_FILE=$started sh "$app/launch.sh"
rc=$?

# A nested update (started from the new version) has already removed
# previous/; then there is nothing to decide here.
if [ -d "$prev" ] && [ ! -e "$started" ]; then
    say "$to exited with $rc before it showed a frame; restoring $from"
    if mv "$app" "$failed" && mv "$prev" "$app"; then
        sync
        printf 'rolled-back %s %s\n' "$to" "$from" >"$result"
        say "restored $from; starting it"
        exec sh "$app/launch.sh"
    fi
    say "restore failed"
    exit 1
fi
say "$to ran (exit status $rc); removing $stage"
rm -rf "$stage"
exit "$rc"
