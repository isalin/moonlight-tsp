# moonlight-tsp

A Moonlight game-streaming client for the TrimUI Smart Pro (Allwinner A133P)
that decodes video in hardware. It streams from Sunshine (or GeForce
Experience) PCs and installs on CrossMix-OS as the app "Moonlight HW".

Other Moonlight builds for this handheld decode in software with FFmpeg and
upload every frame through the GPU. moonlight-tsp sends the stream to the
SoC's Cedar video engine, rotates each picture to the portrait panel with
G2D, and scans it out on a display-engine layer. The CPU never touches the
pixels. Local 720p60 test streams show every frame about 4.5 ms after it is
submitted, at about 7% of one CPU core. In a live stream, Opus audio
decoding is the largest CPU user.

Status: early. Tested on one device with CrossMix-OS 1.3.0. Expect rough
edges.

## Features

- H.264 and HEVC at 1280x720 (the panel's native size), 30 or 60 fps,
  5-30 Mbit/s.
- Hardware decode (Cedar), G2D rotation and a display-engine video layer,
  with no frame of decoder lag.
- Low-latency ALSA audio that keeps the system volume keys working.
- Its own controller-driven UI: PC search (mDNS), add by IP, pairing with a
  PIN, app list, resume and quit the PC's running app, and settings.
- Reuses the pairing of the stock Moonlight app on first run, so a PC that
  is already paired needs no new PIN.
- In-stream menu on MENU: resume, mouse mode, send Guide, disconnect, and
  quit the app and disconnect. Holding START+SELECT+L1+R1 for one second
  disconnects at once.
- Mouse mode, while MENU is held or switched on in the stream menu: right
  stick moves the pointer, left stick scrolls, A/L1/R1 left click, B/L2/R2
  right click, X middle click, Y Enter, d-pad arrow keys.
- The stream engine runs as a separate process with a watchdog, so a hung
  stream returns to the UI with a message instead of freezing the device.
- Leaves the CPU governor alone. Turns Wi-Fi power saving off during
  streams and back on afterwards (a setting).

## Install

1. Download `trimui-moonlight-tsp-<version>.zip`.
2. Extract it at the root of the SD card. This creates
   `Apps/MoonlightTSP/`.
3. Open **Moonlight HW** from the Apps menu.

Settings, keys and logs are kept in `/mnt/UDISK/trimui-tools/moonlight-tsp/`.
To remove the app, delete `Apps/MoonlightTSP/` and that folder.

## Build

Requirements: Docker with `linux/arm64` support (native on Apple silicon,
QEMU elsewhere), plus `zip`, `shasum` and ImageMagick (`magick`) on the
host for packaging.

```sh
sh scripts/fetch-third-party.sh          # pinned sources, SHA-256 checked
sh scripts/docker-build.sh -j8 engine ui # binaries in build/
sh scripts/docker-build.sh check         # host-side tests
sh scripts/package.sh                    # dist/trimui-moonlight-tsp-<version>.zip
```

The build runs in a Debian bullseye container, so the binaries need only
glibc 2.30 and run on the device's glibc 2.33. OpenSSL, curl, expat, uuid
and Opus are linked statically; ALSA and the device's own SDL2/SDL2_ttf
are linked dynamically, and the vendor CedarC libraries are loaded at run
time. Two package builds produce byte-identical zips.
`scripts/ui-screens.sh` renders every UI screen in the container without a
device.

## Layout

| Path | Contents |
|---|---|
| `src/stream_main.c` | `moonlight-tsp-stream`: the stream engine, also a CLI (`info`, `list`, `pair`, `stream`, `quit`, `reset-display`) |
| `src/video_cedar.c`, `cedar*.c`, `g2d_rot.c`, `disp.c`, `ion.c` | video: Cedar decode, G2D rotation, display layer, ION buffers |
| `src/audio_alsa.c`, `src/input_evdev.c`, `src/wifi_ps.c` | audio, controller and mouse input, Wi-Fi power saving |
| `src/ui_*.c` | `moonlight-tsp-ui`: the SDL2 interface |
| `include/` | vendor ABI headers (CedarC 1.3.0, sunxi disp2, G2D) with layout checks |
| `package/` | launcher and icon |
| `tests/` | host tests (`make check`) and the UI screen script |
| `tools/` | device test tools (`tsp-videotest`, `tsp-probe`, `tsp-mdns`) |
| `docs/development-notes.md` | design, measurements and development log |

## License and credits

GPL-3.0-or-later (see `LICENSE`). The stream protocol is
[moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c),
and pairing and the app list use `libgamestream` from
[moonlight-embedded](https://github.com/moonlight-stream/moonlight-embedded).
The exact versions of these and of enet, nanors, OpenSSL and curl are
pinned in `third_party.lock`; their notices ship in the package's
`licenses/` folder. The design was informed by
[moonlight-allwinner-h618](https://github.com/uselesscodestoo/moonlight-allwinner-h618)
(branch `k2b-cedarc-disp`), which shows Cedar-decoded video on a display
layer on the H618 with a newer CedarC and kernel.

This project is not affiliated with the Moonlight project, TrimUI or
CrossMix-OS.
