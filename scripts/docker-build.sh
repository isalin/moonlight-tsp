#!/bin/sh
# Builds moonlight-tsp for AArch64 in a cached Debian bullseye container.
# Usage: scripts/docker-build.sh [make targets...]
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
image=moonlight-tsp-build:bullseye
# Rebuild the image when the Dockerfile changes.
tag=$(shasum -a 256 "$here/docker/Dockerfile" | cut -c1-12)
if [ "$(docker image inspect -f '{{index .Config.Labels "dockerfile"}}' "$image" 2>/dev/null)" != "$tag" ]; then
    docker build --platform linux/arm64 --label dockerfile="$tag" -t "$image" "$here/docker"
fi
mkdir -p "$here/build"
docker run --rm --platform linux/arm64 \
    -v "$here:/src:ro" -v "$here/build:/out" -w /src "$image" \
    make BUILD=/out "$@"
