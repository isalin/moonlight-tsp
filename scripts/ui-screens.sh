#!/bin/sh
# Renders the UI's screens in the build container (no device needed) and
# converts them to PNG in build/shots.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
"$here/scripts/docker-build.sh" ui /out/moonlight-tsp-ui-host
font=${MLTSP_FONT_FILE:-}
fontmount=""
[ -n "$font" ] && fontmount="-v $font:/font.ttf:ro"
# shellcheck disable=SC2086
docker run --rm --platform linux/arm64 -v "$here:/src:ro" \
    -v "$here/build:/out" $fontmount \
    ${font:+-e MLTSP_FONT=/font.ttf} -w /src moonlight-tsp-build:bullseye \
    bash tests/ui-screens.sh
if command -v sips >/dev/null; then
    for f in "$here"/build/shots/*.bmp; do
        sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null
    done
fi
