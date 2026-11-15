#!/bin/sh
# Builds the static OpenSSL and libcurl used by moonlight-tsp-stream.
# Runs inside the build container (make deps); sources come from
# third_party/dl (scripts/fetch-third-party.sh). Output: $DEPS/{include,lib}.
set -eu
src=${SRC:-/src}
deps=${DEPS:-/out/deps}
work=${DEPS_WORK:-/out/deps-build}
jobs=$(nproc)
openssl=openssl-3.5.9
curl=curl-8.23.0

stamp=$deps/.stamp-$openssl-$curl
[ -f "$stamp" ] && { touch "$deps/.built"; exit 0; }
rm -rf "$deps" "$work"
mkdir -p "$deps" "$work"

for t in $openssl $curl; do
    [ -f "$src/third_party/dl/$t.tar.gz" ] || {
        echo "missing third_party/dl/$t.tar.gz: run scripts/fetch-third-party.sh" >&2
        exit 1
    }
    tar -C "$work" -xzf "$src/third_party/dl/$t.tar.gz"
done

# The config directory does not exist on the device, so no system
# openssl.cnf (written for the device's OpenSSL 1.1) is loaded.
cd "$work/$openssl"
./Configure linux-aarch64 --prefix="$deps" --libdir=lib \
    --openssldir=/mnt/UDISK/trimui-tools/moonlight-tsp/openssl \
    no-shared no-module no-dso no-engine no-legacy no-comp no-ssl3 \
    no-weak-ssl-ciphers no-tests no-docs no-apps no-quic no-srp no-psk \
    no-ui-console -O2
make -j"$jobs" build_libs >/dev/null
make install_dev >/dev/null

# HTTP(S) only: libgamestream talks to the host on ports 47989/47984.
cd "$work/$curl"
PKG_CONFIG_PATH=$deps/lib/pkgconfig ./configure --prefix="$deps" \
    --disable-shared --enable-static --with-openssl="$deps" \
    --without-libpsl --without-zlib --without-brotli --without-zstd \
    --without-nghttp2 --without-nghttp3 --without-ngtcp2 --without-libidn2 \
    --without-libssh2 --without-libssh --without-gssapi --without-libgsasl \
    --without-ca-bundle --without-ca-path --without-ca-fallback \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
    --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
    --disable-smtp --disable-gopher --disable-mqtt --disable-file \
    --disable-ftp --disable-ntlm --disable-kerberos-auth \
    --disable-negotiate-auth --disable-aws --disable-docs --disable-manual \
    --disable-unix-sockets --disable-websockets --disable-cookies \
    --disable-alt-svc --disable-hsts --disable-netrc --disable-ipfs \
    --disable-progress-meter --disable-libcurl-option --enable-ipv6 \
    --enable-threaded-resolver >/dev/null
make -j"$jobs" -C lib >/dev/null
make -C lib install >/dev/null
make -C include install >/dev/null
install -m 644 libcurl.pc "$deps/lib/pkgconfig/"

rm -rf "$work"
touch "$stamp" "$deps/.built"
echo "deps: $openssl and $curl in $deps"
