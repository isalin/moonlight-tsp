# Build environment for moonlight-tsp: glibc 2.31 is older than the
# device's 2.33, so binaries built here run on CrossMix-OS.
#
# Bullseye security updates have moved to archive.debian.org; the
# deb.debian.org index still lists them but the pool returns 404.
#
# The base is pinned to the image 0.2.0 was built with. Bullseye's
# security support ended in August 2026, so its packages no longer change.
FROM debian:bullseye@sha256:6f519a81440354a85eb592c5f32109ab80605f6b892455983a6f618bf87fabe9
RUN sed -i 's|http://deb.debian.org/debian-security|http://archive.debian.org/debian-security|' /etc/apt/sources.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates gcc g++ make cmake pkg-config libc6-dev file perl \
      libexpat1-dev uuid-dev libopus-dev libasound2-dev \
      libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core \
 && rm -rf /var/lib/apt/lists/*
