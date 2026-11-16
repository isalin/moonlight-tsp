#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# End-to-end test of "moonlight-tsp-stream update-check/update-fetch"
# against tests/update-server.py (HTTPS with a throwaway CA, GitHub-style
# redirects), then of the launcher's install step (tests/update-apply.sh).
# Runs in the build container: scripts/update-test.sh.
set -u
engine=${ENGINE:-/out/moonlight-tsp-stream}
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d /tmp/update-e2e.XXXXXX)
www=$t/www
stage=$t/sd/.moonlight-tsp-update
fails=0
n=0
server=

cleanup() {
    [ -n "$server" ] && kill "$server" 2>/dev/null
    rm -rf "$t"
}
trap cleanup EXIT

ok() { n=$((n + 1)); echo "ok   $1"; }
bad() { n=$((n + 1)); fails=$((fails + 1)); echo "FAIL $1"; }
# NAME CONDITION...: the condition is a command.
check() {
    local name=$1
    shift
    if "$@"; then ok "$name"; else bad "$name"; fi
}

# ---- certificates: a test CA, a server certificate, an unrelated CA
mkdir -p "$www" "$t/sd"
ssl() { openssl "$@" >>"$t/openssl.log" 2>&1 || { echo "openssl $1 failed";
        cat "$t/openssl.log"; exit 1; }; }
ssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=update-test-ca \
    -keyout "$t/ca.key" -out "$t/ca.pem"
ssl req -newkey rsa:2048 -nodes -subj /CN=127.0.0.1 -keyout "$t/srv.key" \
    -out "$t/srv.csr"
printf 'subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\n' >"$t/ext"
ssl x509 -req -in "$t/srv.csr" -CA "$t/ca.pem" -CAkey "$t/ca.key" \
    -CAcreateserial -days 2 -extfile "$t/ext" -out "$t/srv.pem"
ssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=other-ca \
    -keyout "$t/other.key" -out "$t/other.pem"

python3 -I "$here/update-server.py" "$www" "$t/srv.pem" "$t/srv.key" \
    "$t/port" 2>"$t/server.log" &
server=$!
for _ in $(seq 50); do [ -s "$t/port" ] && break; sleep 0.1; done
[ -s "$t/port" ] || { echo "server did not start"; cat "$t/server.log"; exit 1; }
url="https://127.0.0.1:$(cat "$t/port")/releases"

# ---- an unzip that behaves like BusyBox "unzip -q -o ZIP -d DIR"
cat >"$t/unzip" <<'EOF'
#!/usr/bin/python3 -I
import os, sys, zipfile
a = sys.argv[1:]
assert a[:2] == ["-q", "-o"] and a[3] == "-d", a
with zipfile.ZipFile(a[2]) as z:
    for i in z.infolist():
        p = z.extract(i, a[4])
        if not i.is_dir() and i.external_attr >> 16:
            os.chmod(p, (i.external_attr >> 16) & 0o777)
EOF
chmod 755 "$t/unzip"

# ---- releases
# release VERSION [STUB_VERSION [MISSING_FILE]]: a package whose programs
# answer --version with STUB_VERSION, and its update.txt.
release() {
    local v=$1 sv=${2:-$1} miss=${3:-} d=$t/pkg/$1 a p zip
    a=$d/Apps/MoonlightTSP
    rm -rf "$d"
    mkdir -p "$a" "$www/v$v"
    for p in moonlight-tsp-ui moonlight-tsp-stream; do
        printf '#!/bin/sh\n[ "$1" = --version ] && echo "%s %s"\nexit 0\n' \
            "$p" "$sv" >"$a/$p"
    done
    printf '#!/bin/sh\necho "launch %s"\n' "$v" >"$a/launch.sh"
    cp "$here/../package/update.sh" "$a/update.sh"
    echo '{}' >"$a/config.json"
    chmod 755 "$a"/*
    [ -n "$miss" ] && rm -f "$a/$miss"
    zip=$www/v$v/trimui-moonlight-tsp-$v.zip
    (cd "$d" && python3 -I -c '
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in sorted(os.walk("Apps")):
        for f in sorted(files):
            z.write(os.path.join(root, f))
' "$zip")
    manifest "$v" "$v" "$(stat -c %s "$zip")" \
        "$(sha256sum "$zip" | cut -d' ' -f1)"
}
# manifest RELEASE VERSION SIZE SHA256
manifest() {
    printf '# test\nversion=%s\nzip=trimui-moonlight-tsp-%s.zip\nsize=%s\nsha256=%s\nsummary=Test release %s\n' \
        "$2" "$2" "$3" "$4" "$2" >"$www/v$1/update.txt"
}
latest() { echo "$1" >"$www/latest"; }

# run ARGS...: the engine with the test server; output in $t/out.
run() {
    "$engine" --update-url "${URL:-$url}" --ca-file "${CA:-$t/ca.pem}" \
        --stage "${STAGE:-$stage}" --unzip "$t/unzip" "$@" >"$t/out" 2>&1
    rc=$?
}
# said NAME EXTENDED-REGEX: the engine reported "update REGEX".
said() {
    if grep -qE "^update $2" "$t/out"; then
        ok "$1"
    else
        bad "$1"
        sed 's/^/    | /' "$t/out"
    fi
}
no_stage() { [ ! -e "$stage" ]; }

echo "engine: $("$engine" --version)"

# ---- update-check
run update-check
said "no release yet" 'none -$'
check "no release: status 0" test "$rc" = 0

release 0.2.0
latest 0.2.0
run update-check
said "older release" 'none 0\.2\.0$'

release 0.3.0
latest 0.3.0
run update-check
said "same release" 'none 0\.3\.0$'

release 0.3.1
latest 0.3.1
run update-check
said "newer release" "available 0\\.3\\.1 $(stat -c %s "$www/v0.3.1/trimui-moonlight-tsp-0.3.1.zip") Test release 0\\.3\\.1$"
check "redirects followed" grep -q "GET /redirect/latest/update.txt" "$t/server.log"

CA=$t/other.pem run update-check
said "unknown CA refused" 'error The secure connection to GitHub failed'
check "unknown CA: status 1" test "$rc" = 1

CA=$t/none.pem run update-check
said "no CA file" 'error This device has no root certificates'

URL=http://127.0.0.1:1/releases run update-check
said "plain HTTP refused" 'error The update address does not use HTTPS\.'

URL=https://127.0.0.1:1/releases run update-check
said "nothing listening" 'error The connection to GitHub failed'

mkdir -p "$www/v9.0.0"
echo "<html>not a manifest</html>" >"$www/v9.0.0/update.txt"
latest 9.0.0
run update-check
said "not a manifest" 'error The update information on GitHub is not valid\.'
latest 0.3.1

# ---- update-fetch
run update-fetch 0.3.1
said "fetch: staged" 'staged 0\.3\.1$'
check "fetch: status 0" test "$rc" = 0
said "fetch: progress" 'progress [0-9]+ [0-9]+$'
said "fetch: verify" 'verify$'
check "fetch: ready file" test "$(cat "$stage/ready" 2>/dev/null)" = "0.3.1 0.3.0"
check "fetch: new app" test -x "$stage/new/Apps/MoonlightTSP/moonlight-tsp-stream"
check "fetch: no zip left" test ! -e "$stage/download.zip" -a \
    ! -e "$stage/download.zip.part" -a ! -e "$stage/new.tmp"

touch "$stage/previous" "$stage/failed"
run update-fetch 0.3.1
said "fetch again over a stage" 'staged 0\.3\.1$'
check "fetch again: old stage files gone" test ! -e "$stage/previous" -a \
    ! -e "$stage/failed"
rm -rf "$stage"

release 0.3.2
manifest 0.3.2 0.3.2 "$(stat -c %s "$www/v0.3.2/trimui-moonlight-tsp-0.3.2.zip")" \
    0000000000000000000000000000000000000000000000000000000000000000
run update-fetch 0.3.2
said "wrong sha256" 'error The download is damaged\. Try again\.'
check "wrong sha256: stage removed" no_stage

release 0.3.3
manifest 0.3.3 0.3.3 1000 \
    "$(sha256sum "$www/v0.3.3/trimui-moonlight-tsp-0.3.3.zip" | cut -d' ' -f1)"
run update-fetch 0.3.3
said "longer than the manifest" 'error The download is damaged \(too large\)'
check "too large: stage removed" no_stage

touch "$www/short"
run update-fetch 0.3.1
said "cut-off download" 'error The download stopped\. Make sure that Wi-Fi is on'

check "cut-off: stage removed" no_stage
rm -f "$www/short"

release 0.3.4 0.3.3
run update-fetch 0.3.4
said "programs report another version" 'error Version 0\.3\.4 does not start on this device\.'
check "does not start: stage removed" no_stage

release 0.3.5 0.3.5 update.sh
run update-fetch 0.3.5
said "no update.sh in the zip" 'error The download is not a Moonlight HW package\.'

mkdir -p "$www/v0.3.6"
cp "$www/v0.3.1/update.txt" "$www/v0.3.6/update.txt"
run update-fetch 0.3.6
said "manifest of another version" 'error The update information on GitHub is not valid\.'

mkdir -p "$www/v0.3.7"
head -c 5000 /dev/urandom >"$www/v0.3.7/trimui-moonlight-tsp-0.3.7.zip"
manifest 0.3.7 0.3.7 5000 \
    "$(sha256sum "$www/v0.3.7/trimui-moonlight-tsp-0.3.7.zip" | cut -d' ' -f1)"
run update-fetch 0.3.7
said "not a zip" 'error The download did not unpack'
check "not a zip: stage removed" no_stage

run update-fetch 0.9.9
said "no such release" 'error Version 0\.9\.9 is not on GitHub\.'
check "no such release: no stage made" no_stage

run update-fetch ../0.3.1
said "bad version argument" 'error Version "\.\./0\.3\.1" is not valid\.'

STAGE=$t/sd/elsewhere run update-fetch 0.3.1
said "stage with another name" 'error The stage folder must be called'
check "stage with another name: not made" test ! -e "$t/sd/elsewhere"

# ---- the install step
echo "---- update.sh and launch.sh"
sh "$here/update-apply.sh" "$here/../package" || fails=$((fails + 1))

echo "update-e2e: $n checks, $fails failed"
[ "$fails" = 0 ]
