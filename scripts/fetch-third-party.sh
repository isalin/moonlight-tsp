#!/bin/sh
# Fetches the sources pinned in third_party.lock. Git sources are checked out
# at the exact commit; tarballs must match their SHA-256. Safe to re-run.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
src=$here/third_party/src
dl=$here/third_party/dl
mkdir -p "$src" "$dl"

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
else
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

grep -vE '^[[:space:]]*(#|$)' "$here/third_party.lock" |
while read -r name kind url pin; do
    case $kind in
    git)
        d=$src/$name
        if [ -d "$d/.git" ] && [ "$(git -C "$d" rev-parse HEAD)" = "$pin" ]; then
            echo "$name: ok"
            continue
        fi
        rm -rf "$d"
        git init -q "$d"
        git -C "$d" fetch -q --depth 1 "$url" "$pin"
        git -C "$d" -c advice.detachedHead=false checkout -q FETCH_HEAD
        got=$(git -C "$d" rev-parse HEAD)
        [ "$got" = "$pin" ] || { echo "$name: got $got, want $pin" >&2; exit 1; }
        echo "$name: fetched $pin"
        ;;
    tar)
        f=$dl/${url##*/}
        if [ -f "$f" ] && [ "$(sha256 "$f")" = "$pin" ]; then
            echo "$name: ok"
            continue
        fi
        curl -sSfL -o "$f.part" "$url"
        got=$(sha256 "$f.part")
        if [ "$got" != "$pin" ]; then
            rm -f "$f.part"
            echo "$name: sha256 $got, want $pin" >&2
            exit 1
        fi
        mv "$f.part" "$f"
        echo "$name: fetched ${url##*/}"
        ;;
    *)
        echo "$name: unknown kind $kind" >&2
        exit 1
        ;;
    esac
done
