# moonlight-tsp agent guide

Read [README.md](README.md) and
[docs/development-notes.md](docs/development-notes.md) first. The author's
general TrimUI device notes and the Moonlight Wi-Fi (OBSS) fix live in a
separate repository (locally `../trimui`, see its `AGENTS.md` and
`docs/device-notes.md`); this client's launcher joins that fix's session
contract when the fix is installed.

## Access and safety

- The development device is a TrimUI Smart Pro running CrossMix-OS, reached
  with `ssh trimui`. The target shell is a BusyBox `ash` shell running as
  `root` on host `TSP`.
- The SSH connection itself uses `wlan0`. Do not bring `wlan0` down, restart
  Wi-Fi, kill `wpa_supplicant`/`udhcpc`, reboot, change the associated
  network, or alter power-saving state during a remote session without
  explicit user approval and a recovery plan. Disconnecting Wi-Fi ends
  remote access.
- Prefer passive observations. Never print Wi-Fi PSKs, Moonlight private
  keys, pairing certificates, or cookies. Do not print anything from
  `/.cache/moonlight` or from the app's `keys/` folder.
- The device is embedded Linux: `ip` is the BusyBox version, `file` and
  `jq` are missing, and `logread` has no readable buffer. The SD card is
  `/mnt/SDCARD`; internal user data is `/mnt/UDISK`. The device clock is set
  to CST (UTC+8), so its log times read ahead of the user's local time.
- Never run the Wi-Fi fix's `obss_filter_test` on the device: it rewrites
  and deletes the live frequency file.
- Do not change the CPU governor or frequency limits; the launcher must not
  set them either.

## Device tests

- Tests that put a display-engine layer on screen (`tsp-videotest -d`) take
  over the screen. Tell the user before running them, keep them short, and
  keep the `-X` reset command ready (`tsp-videotest -X -F -C 0 -L 0` flushes
  the display driver's buffer imports, then disables channel 0, layer 0).
  Never leave a layer pointing at freed decoder buffers, and do not show
  video through v1 (address) layers outside short experiments: a dead
  process leaves them scanning unmapped memory.
- Check `/sys/kernel/debug/ion/heaps/sys_user` (orphaned buffers) and the
  `dmabuf: cache` field of `/sys/class/disp/disp/attr/sys` before and after
  display tests.
- Streams to the user's PC (Sunshine host) start apps there; run them only
  with the user's go-ahead.
- The engine turns Wi-Fi power save off for each stream and back on
  afterwards (the user chose this; `--keep-wifi-ps` and a UI setting
  disable it). A killed engine leaves `/tmp/moonlight-tsp-wifi-ps-off`, and
  `reset-display` restores power save. Outside the engine, changing power
  save still needs the user's approval and an auto-revert timer.
- Spike binaries live in `/tmp/tsp-spike/` on the device and vanish on
  reboot.

## Install and logs

- The app is installed at `/mnt/SDCARD/Apps/MoonlightTSP/` (label
  "Moonlight HW") from `dist/trimui-moonlight-tsp-<version>.zip`, built by
  `scripts/package.sh`. Install by copying the zip to `/tmp`, checking its
  SHA-256 and `unzip -o -d /mnt/SDCARD`. If `pidof moonlight-tsp-ui` shows
  it running, extract to a stage folder on the SD card and `mv` changed
  files into place instead (see "Package" in the notes).
- Let the user launch it from the Apps menu: running the UI over SSH while
  MainUI owns the screen would fight over the framebuffer.
- Logs are in `/mnt/UDISK/trimui-tools/moonlight-tsp/logs/`. If the UI's
  watchdog kills a hung engine it first writes `logs/hang.log` (threads,
  syscalls, kernel stacks, code addresses, poll sets, open descriptors);
  read it before guessing at a cause.

## Releases and updates

- Every installed copy checks
  `github.com/isalin/moonlight-tsp/releases/latest/download/update.txt`
  at start and offers the newest release. Thus a published release reaches
  all users. Publish (`scripts/release.sh VERSION --publish`) only with the
  user's OK, from a clean, committed `master`, after the package was
  installed and started on the device. A dry run
  (`scripts/release.sh VERSION`) publishes nothing.
- The installed version runs the update, so later versions must keep
  these working: `moonlight-tsp-stream --version` and
  `moonlight-tsp-ui --version` print `NAME VERSION`; the zip holds
  `Apps/MoonlightTSP/` with `launch.sh`, `update.sh`, `config.json` and
  both programs; the UI exits with status 3 to install a staged update and
  writes `$MLTSP_STARTED_FILE` after its first frame; `update.sh apply APP
  STAGE DATA` keeps its arguments; the release has the assets
  `update.txt` and `trimui-moonlight-tsp-VERSION.zip`, and `update.txt`
  keeps its `key=value` format.
- Do not delete or replace a published release asset: copies that are
  downloading it would fail. Fix a bad release with a newer version.
- The stage folder is `/mnt/SDCARD/.moonlight-tsp-update`. Test the
  updater with `scripts/update-test.sh` (container) after any change to
  `src/update*.c`, `package/launch.sh` or `package/update.sh`;
  `tests/update-apply.sh` is also safe to run on the device from `/tmp`.

## Code rules

- The executable exports its own `SemTimedWait` to replace the vendor
  polling version. Keep `-Wl,--export-dynamic-symbol=SemTimedWait` on every
  binary that loads CedarC, and check that the `semwait:` line says
  `interposed`.
- The engine also exports its own `close()` (`src/fdguard.c`) to refuse and
  log stray closes of its socket, timer and pipes. Keep
  `-Wl,--export-dynamic-symbol=close` on the engine, close those
  descriptors only through `fdguard_close()`, and check that the `fdguard:`
  line says `interposed`. Any `fdguard: refused` or `was replaced` line in
  `stream.log` is a real bug to trace.
- In the UI, compute elapsed times with `ms_since(now, t)`, never a plain
  unsigned `now - t`: handlers stamp events with a fresh `now_ms()` that can
  be a millisecond ahead of the loop's `now`. That wrap made the watchdog
  kill healthy streams.
- Input (`src/input_evdev.c`) has two modes: controller, and mouse mode
  (MENU held, or "Mouse mode: On" in the stream menu), which sends mouse and
  keyboard events to the PC. All its state is guarded by `I.lock`; change it
  only under that lock and through `sync_host()`, and keep
  `tests/input-test.c` (`make check`) passing after any input change.
- Keep the package reproducible: two runs of `scripts/package.sh` must give
  the same zip hash, and a fresh clone must build the same binaries. Keep
  every build input pinned (`third_party.lock`, the base image digest in
  `docker/Dockerfile`, `SOURCE_DATE_EPOCH` in `scripts/build-deps.sh`);
  anything that embeds a date or a host path breaks this. The UI and
  engine versions (`UI_VERSION`, `ENGINE_VERSION`) must match.

Update `docs/development-notes.md` whenever new device facts or diagnostic
results are learned.
