#!/bin/sh
# Generates 720p60 no-B-frame Annex-B test streams with access unit
# delimiters (one AUD per frame), similar to what Sunshine sends.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/build/media"
secs=${1:-20}
mkdir -p "$out"
# The top-left 64x64 block is flat luma 16 + 8 * (frame % 27), so a tool
# can read the frame number back from a decoded picture (tsp-videotest -M).
src="testsrc=size=1280x720:rate=60,format=yuv420p[bg];\
color=c=black:size=64x64:rate=60,format=yuv420p,\
geq=lum='16+8*mod(N\\,27)':cb=128:cr=128[mark];\
[bg][mark]overlay=0:0:shortest=1,format=yuv420p"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$src" -t "$secs" \
    -c:v libx264 -preset veryfast -tune zerolatency -profile:v high \
    -bf 0 -g 600 -b:v 10M -maxrate 10M -bufsize 1M \
    -color_primaries smpte170m -color_trc smpte170m -colorspace smpte170m \
    -bsf:v h264_metadata=aud=insert -f h264 "$out/test720p60.264"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$src" -t "$secs" \
    -c:v libx265 -preset veryfast -tune zerolatency \
    -x265-params "bframes=0:keyint=600:log-level=error" -b:v 10M \
    -color_primaries smpte170m -color_trc smpte170m -colorspace smpte170m \
    -bsf:v hevc_metadata=aud=insert -f hevc "$out/test720p60.265"
ls -l "$out"
