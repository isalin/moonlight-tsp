#!/bin/sh
# Builds the Moonlight HW (moonlight-tsp) app package for CrossMix-OS:
# dist/trimui-moonlight-tsp-<version>.zip, extracted at the SD-card root as
# Apps/MoonlightTSP/. Two runs give byte-identical archives.
set -eu

src=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
image=moonlight-tsp-build:bullseye
version=$(sed -n 's/^#define UI_VERSION "\(.*\)"$/\1/p' "$src/src/ui_main.c")
engine_version=$(sed -n 's/^#define ENGINE_VERSION "\(.*\)"$/\1/p' \
    "$src/src/stream_main.c")
if [ -z "$version" ] || [ "$version" != "$engine_version" ]; then
    echo "UI ($version) and engine ($engine_version) versions differ" >&2
    exit 1
fi
build="$src/build"
stage="$build/package"
app="$stage/Apps/MoonlightTSP"
dist="$src/dist"
archive="$dist/trimui-moonlight-tsp-$version.zip"

sh "$src/scripts/docker-build.sh" -j8 engine ui
sh "$src/scripts/docker-build.sh" check

rm -rf "$stage"
mkdir -p "$app/licenses" "$app/source" "$dist"

# Strip, check the binaries, gather license notices and the source archive
# with the container's GNU tools.
docker run --rm --platform linux/arm64 \
    -v "$src:/src:ro" -v "$build:/out" -w /src -e VERSION="$version" "$image" \
    sh -eu -c '
app=/out/package/Apps/MoonlightTSP
strip -o "$app/moonlight-tsp-stream" /out/moonlight-tsp-stream
strip -o "$app/moonlight-tsp-ui" /out/moonlight-tsp-ui

# CedarC loads through the engine: its SemTimedWait must stay exported.
nm -D --defined-only "$app/moonlight-tsp-stream" | grep -q " T SemTimedWait$" || {
    echo "engine does not export SemTimedWait" >&2; exit 1; }
# Only glibc, ALSA and the device SDL2/SDL2_ttf may be dynamic.
for b in moonlight-tsp-stream moonlight-tsp-ui; do
    readelf -d "$app/$b" | sed -n "s/.*(NEEDED).*\[\(.*\)\]/\1/p" |
        while read -r lib; do
            case $lib in
            libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|\
            ld-linux-aarch64.so.1|libasound.so.2|libSDL2-2.0.so.0|\
            libSDL2_ttf-2.0.so.0) ;;
            *) echo "$b: unexpected dependency $lib" >&2; exit 1 ;;
            esac
        done
done

l=$app/licenses
tp=/src/third_party/src
cp "$tp/moonlight-common-c/LICENSE.txt" "$l/moonlight-common-c.txt"
cp "$tp/moonlight-embedded/LICENSE" "$l/moonlight-embedded-libgamestream.txt"
cp "$tp/enet/LICENSE" "$l/enet.txt"
cp "$tp/nanors/LICENSE" "$l/nanors.txt"
tar -xzOf /src/third_party/dl/openssl-3.5.9.tar.gz openssl-3.5.9/LICENSE.txt \
    >"$l/openssl.txt"
tar -xzOf /src/third_party/dl/curl-8.23.0.tar.gz curl-8.23.0/COPYING \
    >"$l/curl.txt"
cp /usr/share/doc/libexpat1-dev/copyright "$l/expat.txt"
cp /usr/share/doc/uuid-dev/copyright "$l/libuuid.txt"
cp /usr/share/doc/libopus-dev/copyright "$l/opus.txt"
cp /src/LICENSE "$app/LICENSE"

# Our sources plus the pinned GPL/MIT trees. OpenSSL and curl are fetched
# from the URLs and SHA-256 sums in third_party.lock.
tar --sort=name --mtime=@1767225600 --owner=0 --group=0 --numeric-owner \
    --mode=go-w --exclude=.git --exclude=.DS_Store --exclude=./build \
    --exclude=./dist --exclude=./.claude --exclude=./third_party/dl \
    --transform "s,^\.,moonlight-tsp-$VERSION," -C /src -cf - . |
    gzip -n -9 >"$app/source/moonlight-tsp-$VERSION-src.tar.gz"
'

cp "$src/package/launch.sh" "$app/launch.sh"
cp "$src/package/update.sh" "$app/update.sh"
cat >"$app/config.json" <<EOF
{
    "label": "Moonlight HW",
    "icon": "icon.png",
    "launch": "launch.sh",
    "description": "Moonlight $version with hardware video decoding"
}
EOF
cat >"$app/README.txt" <<EOF
Moonlight HW $version (moonlight-tsp) for the TrimUI Smart Pro

Streams games from a Sunshine (or GeForce Experience) PC. Video is decoded by
the Allwinner video engine and shown on a display-engine layer, so the CPU
stays mostly idle during a stream.

During a stream: MENU opens the stream menu. Holding START+SELECT+L+R for one
second disconnects at once.

The pairing of the stock Moonlight app is copied on first use, so a PC paired
with it does not need a new PIN. Settings, keys and logs are kept in
/mnt/UDISK/trimui-tools/moonlight-tsp/ (logs/ has the last two runs).

Updates: when the app starts, it asks GitHub (github.com/isalin/moonlight-tsp)
for a newer version and offers to install it. Turn this off in Settings
("Check for updates at start"); "Check for updates now" checks at any time.
If an update is cut off by a power loss and this folder is missing, rename
/.moonlight-tsp-update/previous on the SD card back to Apps/MoonlightTSP.

The launcher never changes CPU governor or frequency settings. It takes part
in the Moonlight Wi-Fi fix's session contract when that fix is installed.

Remove: delete this folder and /mnt/UDISK/trimui-tools/moonlight-tsp/.

License: GPL-3.0-or-later (LICENSE). Third-party notices are in licenses/;
the complete source is in source/.
EOF
magick -background none "$src/package/icon.svg" -resize 300x300 -depth 8 \
    -strip "$app/icon.png"

find "$stage" -type d -exec chmod 755 {} +
find "$stage" -type f -exec chmod 644 {} +
chmod 755 "$app/launch.sh" "$app/update.sh" "$app/moonlight-tsp-stream" \
    "$app/moonlight-tsp-ui"
find "$stage" -exec touch -t 202601010000 {} +

rm -f "$archive"
(
    cd "$stage"
    find Apps -type f -print | LC_ALL=C sort | zip -X -q "$archive" -@
)
echo "Built $archive"
shasum -a 256 "$archive"
