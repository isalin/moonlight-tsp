# Changelog

The first line of each section is its summary, at most 60 characters. The
app shows that line when it offers the update. `scripts/release.sh` puts
the section into the GitHub release notes.

## 0.3.1

Shows battery and Wi-Fi status at the top right

- The top right of each menu and of the Paused screen shows the battery
  level and the Wi-Fi signal. The battery icon is green with a lightning
  bolt when the handheld charges. It is yellow at 20% or less and red at
  10% or less.
- The app reads these values every 5 seconds in the menus and never
  during a stream. To get the Wi-Fi signal, the app reads the data that
  the Wi-Fi driver already has. It does not start a Wi-Fi scan.
- The CPU Performance message is shorter, so that it fits next to the
  status icons.

## 0.3.0

The app can now update itself from GitHub

- When the app starts, it asks GitHub for a newer version. If there is
  one, it shows the version and its summary. You can update, wait until
  later, or skip that version. To stop these checks, set "Check for
  updates at start" to Off in Settings.
- "Check for updates now" in Settings checks at any time.
- The app downloads the update to the SD card and checks its size and
  SHA-256. It also makes sure that the new programs start on the
  handheld. Then the app replaces its folder and starts again. If the
  new version does not start, the old version comes back.
- The settings list scrolls and has a scroll bar.
- `moonlight-tsp-ui --version` and `moonlight-tsp-stream --version`.

## 0.2.0

First public version

- Hardware video decoding (Cedar) with G2D rotation and a display-engine
  layer, ALSA audio with low latency, and a UI made for the controller.
- Search for PCs, add a PC by IP address, pairing with a PIN, and the
  pairing data of the stock Moonlight app.
- Stream menu, mouse mode, and Wi-Fi power saving off during a stream.
