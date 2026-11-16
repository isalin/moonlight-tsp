#!/bin/sh
# Makes a release of moonlight-tsp.
#
#   scripts/release.sh VERSION             build and check (dry run)
#   scripts/release.sh VERSION --publish   also tag, push and publish
#
# A release vVERSION has two assets: the app zip and update.txt, the
# manifest that installed apps read through
#   https://github.com/isalin/moonlight-tsp/releases/latest/download/update.txt
# Each published release reaches every installed app that checks for
# updates. Test the zip on a device before you publish it.
set -eu

src=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
version=${1:-}
publish=0
case ${2:-} in
"") ;;
--publish) publish=1 ;;
*) echo "usage: scripts/release.sh VERSION [--publish]" >&2; exit 2 ;;
esac
case $version in
""|.*|*.|*..*|*[!0-9.]*)
    echo "usage: scripts/release.sh VERSION [--publish]" >&2
    echo "VERSION is dotted numbers, for example 0.3.0" >&2
    exit 2 ;;
esac
die() { echo "release: $*" >&2; exit 1; }

ui=$(sed -n 's/^#define UI_VERSION "\(.*\)"$/\1/p' "$src/src/ui_main.c")
engine=$(sed -n 's/^#define ENGINE_VERSION "\(.*\)"$/\1/p' \
    "$src/src/stream_main.c")
[ "$ui" = "$version" ] && [ "$engine" = "$version" ] ||
    die "UI_VERSION ($ui) and ENGINE_VERSION ($engine) must be $version"

# The CHANGELOG section: its first line is the summary.
notes=$src/dist/release-notes-$version.md
mkdir -p "$src/dist"
awk -v v="## $version" '
    $0 == v { f = 1; next }
    /^## / { f = 0 }
    f' "$src/CHANGELOG.md" | sed -e '/./,$!d' >"$notes.tmp"
summary=$(sed -n '1p' "$notes.tmp")
[ -n "$summary" ] || die "CHANGELOG.md has no section \"## $version\""
[ "$(printf '%s' "$summary" | wc -c)" -le 60 ] ||
    die "the summary of $version is longer than 60 characters: $summary"
{
    cat "$notes.tmp"
    cat <<NOTES

---

**Install:** extract \`trimui-moonlight-tsp-$version.zip\` to the root of
the SD card (see the README). Version 0.3.0 and later offer new versions
when they start. \`update.txt\` is for the app's update check.
NOTES
} >"$notes"
rm -f "$notes.tmp"

dirty=$(git -C "$src" status --porcelain)
if [ "$publish" = 1 ]; then
    [ -z "$dirty" ] || die "the work tree has changes; commit them first"
    [ "$(git -C "$src" symbolic-ref --short HEAD)" = master ] ||
        die "publish from master"
    if git -C "$src" rev-parse -q --verify "refs/tags/v$version" >/dev/null
    then
        die "tag v$version exists"
    fi
elif [ -n "$dirty" ]; then
    echo "release: note: the work tree has changes (dry run only)"
fi

sh "$src/scripts/package.sh"
zip=$src/dist/trimui-moonlight-tsp-$version.zip
size=$(wc -c <"$zip" | tr -d ' ')
sha=$(shasum -a 256 "$zip" | cut -d' ' -f1)
manifest=$src/dist/update.txt
cat >"$manifest" <<MANIFEST
# moonlight-tsp release manifest (scripts/release.sh)
version=$version
zip=trimui-moonlight-tsp-$version.zip
size=$size
sha256=$sha
summary=$summary
MANIFEST
echo
cat "$manifest"
echo

if [ "$publish" = 0 ]; then
    echo "Dry run. Release notes: $notes"
    echo "To publish: scripts/release.sh $version --publish"
    exit 0
fi

git -C "$src" tag -a "v$version" -m "moonlight-tsp $version"
git -C "$src" push origin master "v$version"
gh release create "v$version" "$zip" "$manifest" --repo isalin/moonlight-tsp \
    --verify-tag --latest --title "moonlight-tsp $version" \
    --notes-file "$notes"
echo "Published v$version."
