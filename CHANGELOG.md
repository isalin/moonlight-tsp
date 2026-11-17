# Changelog

The first line of each section is its summary, at most 60 characters. The
app shows that line when it offers the update. `scripts/release.sh` puts
the section into the GitHub release notes.

## 0.4.0

Rumble, a Send key list, PC sound and mouse settings

- The handheld rumbles when the game rumbles the pad. "Rumble strength"
  in Settings sets the strength: Off, or 5% to 100% in steps of 5%. Press
  A on that row for a 0.5 s test rumble at the selected strength. The
  built-in motor can only be on or off, so the app turns it on and off
  fast to make weak and strong rumble. The motor stops when you open the
  stream menu and when the stream ends.
- "Send key" in the stream menu sends a key or a key combination to the
  PC: Esc, Tab, Enter, Windows, Alt+Tab, Alt+F4, Alt+Enter, Win+D, Win+G,
  Ctrl+Shift+Esc, Print Screen and F11. The stream continues at once.
- New setting "Play the sound on the PC too": the speakers of the PC also
  play the sound.
- New settings "Mouse pointer speed" (50% to 200%) and "Mouse scroll
  direction" (Normal or Reverse) for mouse mode.
- The stream menu shows a help text for the selected item.
- The settings have a new order: stream, controls, sound, Wi-Fi, PC and
  updates.
- `moonlight-tsp-stream rumble-test [PCT]` makes one 0.5 s rumble.

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
