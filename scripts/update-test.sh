#!/bin/sh
# Tests the updater end to end in the build container: the engine's
# update-check/update-fetch against a local HTTPS stand-in for GitHub, then
# the launcher's install and rollback (tests/update-e2e.sh).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
"$here/scripts/docker-build.sh" -j8 engine
docker run --rm --platform linux/arm64 -v "$here:/src:ro" \
    -v "$here/build:/out" -w /src moonlight-tsp-build:bullseye \
    bash tests/update-e2e.sh
