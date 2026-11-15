# moonlight-tsp

A Moonlight game-streaming client for the TrimUI Smart Pro (Allwinner A133P)
that decodes video in hardware. It streams from Sunshine (or GeForce
Experience) PCs and installs on CrossMix-OS as the app "Moonlight HW".

Other Moonlight builds for this handheld decode in software with FFmpeg and
upload every frame through the GPU. moonlight-tsp sends the stream to the
SoC's Cedar video engine, rotates each picture to the portrait panel with
G2D, and scans it out on a display-engine layer. The CPU never touches the
pixels.

Status: early. Tested on one device with CrossMix-OS 1.3.0. Expect rough
edges.

## Compared with the stock client

CrossMix-OS ships Moonlight Embedded 2.6.1 (2023) behind TrimUI's own
front end. PortMaster's Moonlight New also decodes in software.

| | Stock client | moonlight-tsp |
|---|---|---|
| Video decoding | Software (FFmpeg) on the CPU, then every frame is uploaded to the GPU | The SoC's Cedar video engine; G2D rotates the picture and a display layer shows it |
| CPU clock | The launcher pins the CPU at 1.6 GHz (`performance` governor) | Leaves the CPU settings alone |
| Audio | Through the system mixer, about 85 ms of buffering | Straight to the codec in 5 ms periods with a 40 ms buffer; the volume keys still work |
| Moonlight code | Moonlight Embedded 2.6.1 | moonlight-common-c and libgamestream from November 2026, with OpenSSL 3.5 |

Measured on the device at 1280x720 and 60 fps:

- Local test streams: each frame is on screen about 4.5 ms after it is
  submitted, with no frames of decoder lag, at about 7% of one CPU core.
  A 10-minute run showed no memory growth.
- Live stream from Sunshine: about 19% of one core for the whole client.
  Audio is the largest part, about 9% (measured on the slower shared audio
  path).
- Wi-Fi power saving, which the client turns off during a stream: in a
  test during video, turning it off cut the average ping to the router
  from 59 ms to 12.5 ms and the longest stall from 1.8 s to 0.13 s.

Not measured yet: a side-by-side stream against the stock client, so there
are no figures for how much CPU time or battery it saves.

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

## Requirements

- A TrimUI Smart Pro running CrossMix-OS. Only CrossMix-OS 1.3.0 has been
  tested.
- A PC running [Sunshine](https://github.com/LizardByte/Sunshine) (the only
  host tested) on the same network as the handheld.

## Install

1. Download the latest `trimui-moonlight-tsp-<version>.zip` from the
   Releases page.
2. Turn the handheld off, take out the SD card and put it in your
   computer.
3. Extract the zip at the root of the SD card. It holds
   `Apps/MoonlightTSP/`, so the app lands in the card's `Apps` folder. If
   your unzip tool puts everything in an extra folder named after the zip,
   move `MoonlightTSP` into the card's `Apps` folder yourself.
4. Put the card back, turn the handheld on and open **Moonlight HW** from
   the Apps menu.

To update, install a newer zip the same way and replace the old files.
Settings, pairing and logs are kept on the handheld's internal storage in
`/mnt/UDISK/trimui-tools/moonlight-tsp/`, so updates keep them.

To remove the app, delete `Apps/MoonlightTSP/` from the SD card and
`/mnt/UDISK/trimui-tools/moonlight-tsp/` from internal storage (for
example over SSH). The PC still lists the handheld as paired until you
remove it in Sunshine.

## First use

1. **Add your PC.** With no saved PCs, the app opens **Search for PCs**.
   It lists the Sunshine PCs on your network and shows whether each one is
   paired. Select yours and press A to add it. If it is not listed, press
   B and choose **Add PC by IP address** in the PC list: A types the
   selected key, Y deletes, START saves.
2. **Pair.** In the PC list, select the PC and press A. If it is not
   paired yet, the app shows a four-digit PIN and the PC's name and
   address. Open that PC's Sunshine web interface, go to its PIN page and
   enter the PIN (any device name works). The app waits up to 5 minutes
   and goes on by itself once the PC accepts the PIN.
3. **Stream.** The PC's app list opens. Select a game or app and press A.
   If a different app is already running on the PC, you are asked before
   it is quit.

If the stock Moonlight app was paired with a PC, Moonlight HW copies that
pairing on first use: the PC shows as paired in the search and needs no
PIN.

| Where | Buttons |
|---|---|
| PC list | A open, X settings, Y remove the PC from the list, SELECT check the PCs again, B leave the app |
| App list | A start or resume, Y quit the app running on the PC, X settings, B back |
| Stream | Press MENU for the stream menu (B resumes). Hold MENU to use the pad as a mouse. Hold START+SELECT+L1+R1 for one second to disconnect at once. |

Settings, with the default first: frame rate (60 fps, or 30), bitrate
(10 Mbit/s; 5 to 30), video codec (Automatic, HEVC or H.264), swap A/B and
X/Y during streams (off: buttons act by position, like an Xbox pad), quit
the PC app on disconnect (off), Wi-Fi power saving off while streaming
(on, recommended), and let the PC optimise game settings (off).

## Troubleshooting

- **Pairing fails, or Sunshine says no device is waiting to pair:** the
  request goes only to the PC named on the PIN screen. Enter the PIN in
  that PC's Sunshine within 5 minutes, then open the PC again to retry.
- **A PC shows as offline:** check that the PC is on and Sunshine is
  running. Press A on it to try again, or SELECT to check every PC.
- **"CPU Performance mode is on: 2 GHz, shorter battery life"** at the
  top of the PC list: the handheld's CPU Performance scene is on and holds
  the CPU at 2 GHz while any app runs. Moonlight HW never changes CPU
  settings itself, so turn the scene off on the handheld if you want
  longer battery life.
- **A stream stops with a message:** the message gives the reason. A
  stream stops by itself about 10 seconds after the PC becomes
  unreachable. If the stream process stops responding, a watchdog closes
  it and writes `hang.log` instead of letting the handheld freeze.
- **Logs** are in `/mnt/UDISK/trimui-tools/moonlight-tsp/logs/`:
  `ui.log`, `launcher.log`, `engine.log` and `stream.log`, each with a
  `.1` copy from the previous run, and `hang.log` if the watchdog closed a
  stream. Please attach them to bug reports.

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
time. The build is reproducible: OpenSSL's build date and the Debian base
image are pinned, so a clean checkout rebuilds the released binaries byte
for byte, and two package builds give identical zips. The icon is rendered
by the host's ImageMagick, so a different version of it can change
`icon.png`.
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
