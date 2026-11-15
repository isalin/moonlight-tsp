# moonlight-tsp: hardware-decoded Moonlight for the TrimUI Smart Pro

Status (2026-11-15): phase 3 (engine) is done for the command line. Live
streams with video, audio and input run against the user's Sunshine host, and
every exit path has been tested (see "Live stream results"). Phase 4 (UI,
launcher, package) is built and tested in the container, and package 0.2.0 is
installed as `/mnt/SDCARD/Apps/MoonlightTSP/` ("Moonlight HW"). Streams from
the app work since the watchdog clock fix (see "Streams after the clock
fix"). 0.2.0 adds mouse mode (hold MENU, or turn it on in the stream menu);
it has been used on the device since 2026-11-15.

This file holds the design, the measurements and a log of the development
on the device. Build with `scripts/docker-build.sh`; outputs go to
`build/`. Until 2026-11-15 the project lived in a `moonlight-tsp/`
folder of the author's TrimUI notes repository, next to the Moonlight
Wi-Fi (OBSS) fix and the device notes; older entries below may still
refer to that layout.

## Why

The stock client (Moonlight Embedded 2.6.1 behind TrimUI's `moonlightui`) and
PortMaster's Moonlight New both decode video in software with FFmpeg and upload
every frame to an SDL2/GLES texture. The stock launcher also forces the
`performance` governor. The A133P has an unused Cedar video engine and a
display engine (DE) that can scan out YUV directly, so a hardware path should
cut CPU load, power, and latency.

## Hardware and vendor stack facts

- Vendor CedarC 1.3.0: `libvdecoder.so`, `libvideoengine.so`, `libsbm.so`,
  `libcdc_base.so`, `libMemAdapter.so`, `libVE.so`, plus codec plugins
  `libawh264.so` and `libawh265.so`. `libvdecoder.so` and `libsbm.so` ship
  DWARF debug info; `libsbm.so` was built from
  `.../a133-aw3/.../libcedarc/vdecoder/sbm/`.
- The codec plugins export `CedarPluginVDInit`, which calls
  `VDecoderRegister`. Load `libvdecoder` and `libvideoengine` with
  `RTLD_GLOBAL`, then `AddVDPluginSingle` only the H.264/H.265 plugins.
- `include/cedarc_abi.h` holds the DWARF-derived structs with `_Static_assert`
  offsets (`VConfig` 0xd0, `VideoStreamInfo` 0x40, `VideoStreamDataInfo` 0x40,
  `VideoPicture` 0x150 with `nLineStride` at 0x14, `pData0` at 0x50 and
  `nBufFd` at 0xa8).
- `DecodeVideoStream` results: 0 ok, 1 frame decoded, 2 continue, 3 keyframe
  decoded, 4 no frame buffer, 5 no bitstream, 6 resolution change, negative on
  error.
- Decoded pictures use internal buffers: `nBufFd` is 0 (no DMA-BUF), but
  `phyYBufAddr`/`phyCBufAddr` are IOMMU addresses (IOVAs).
- The sunxi IOMMU driver uses one global domain ("we just use one domain" in
  `drivers/iommu/sunxi-iommu.c`), so Cedar IOVAs are valid for the DE. The UI
  framebuffer itself sits at IOVA 0xfb800000.
- `/dev/disp` (disp2, kernel 4.9) has two layer interfaces:
  - v1 `DISP_LAYER_SET_CONFIG` 0x47 / `GET_CONFIG` 0x48 take buffer addresses
    (`disp_fb_info` 128 bytes, `disp_layer_info` 168, `disp_layer_config` 184).
  - v2 `DISP_LAYER_SET_CONFIG2` 0x49 / `GET_CONFIG2` 0x4a take DMA-BUF fds.
  - Arguments are `{screen, ptr, count, 0}`. `tools/tsp-probe.c` reads both
    and verified the v1 layout on the device: ch1/lyr0 enabled, z 16, window
    720x1280, address 0xfb800000, format 0, color space 0x104, crop 720x1280.
  - moonlight-tsp uses v2 with the dmabuf fds of its own rotated buffers; see
    "Display path" below for why not v1.
- DE channels: ch0-1 are video (YUV) channels, ch2-3 UI. The framebuffer owns
  ch1/lyr0, so ch0 is free. `de_al_lyr_apply` sorts the used channels by
  `zorder` (lowest gets pipe 0), so ch0 with z 32 is above the framebuffer.
- The panel is physically 720x1280 portrait at about 60.8 Hz; apps see a
  rotated 1280x720 framebuffer. The DE does not rotate video layers, so video
  is rotated by G2D (see "Rotation").
- The A133 4.9 BSP has no `DISP_HWC_COMMIT` release fences, so picture
  release must follow a time/vsync policy.
- `disp_open` and `disp_release` are empty: a layer outlives the process that
  set it. Recovery after a crash is `tsp-videotest -X -F -C 0 -L 0` (flush the
  driver's imports, then disable the layer).

## Decode measurements (decode only, nothing on screen)

Local 720p60 test streams, no B-frames, one AUD per frame
(`scripts/make-test-streams.sh`, 10 Mb/s). Paced at 60 fps.

- Decoder open about 16 ms. RSS grows about 100-120 KiB once per run and not
  with run time. CMA and the ION heaps return to the same values after runs
  (CmaFree 63168 kB, no orphaned ION buffers).
- Hardware decode call: H.264 about 3.3 ms, HEVC about 2.7 ms per 720p frame.
- With the chosen submit path below (150-second, 9002-frame runs): no errors,
  no frame errors, every submission produced a picture, H.264 3.45 ms mean
  submit-to-picture at 3.2% of one core, HEVC 2.78 ms at 2.1%.

### Lag measurement

Picture PTS values are rewritten or interpolated by the decoder, so they
cannot identify which input frame a picture came from. The test streams
instead carry a 64x64 top-left block with luma `16 + 8 * (frame % 27)`, and
`tsp-videotest -M` reads it back from each decoded picture to count how many
frames behind the newest submission the picture is.

### Why the vendor path lags two frames

1. **Frame boundary.** The SBM ("stream buffer manager") parser in
   `libsbm.so` splits the byte stream into frames on its own thread. It only
   closes a frame when it sees the start of the next access unit, so with one
   frame per submission it is always one frame behind.
   - HEVC: the detector treats VPS..AUD and prefix SEI as the start of an AU,
     so appending an AUD (`00 00 00 01 46 01 50`) closes the frame at once.
   - H.264: the A133 detector does not accept AUD, SEI, filler,
     end-of-sequence or end-of-stream as a frame end, but it does accept SPS
     or PPS. Appending the stream's own PPS (8 bytes for the test stream)
     closes the frame at once. Cedar decodes the result without errors.
2. **Parser wake-up.** `SemTimedWait` in `libcdc_base.so` does not block. It
   polls `sem_trywait` with `usleep` backoff starting at 10 us and doubling up
   to the timeout (20 ms in the parser). A frame submitted while the parser is
   sleeping waits for the next poll, up to about 10 ms, so `DecodeVideoStream`
   called right after submit usually finds nothing and the frame is decoded
   on the next call instead. The polling also wakes the CPU hundreds of times
   per second.

Measured at 60 fps (H.264 unless noted):

| Submit path | Frames behind | Submit to picture |
|---|---|---|
| Vendor default | 2 (both codecs) | 3.3 ms + 2 frames (about 36 ms) |
| PPS suffix, poll decode every 200 us, vendor wait | 0 for 88%, else 1 | 7.4 ms mean, 12 ms p99 |
| PPS suffix, vendor wait, wait for parser | 0 for 88%, else 1 | 11.9 ms mean |
| PPS suffix, blocking wait, wait for parser | 0 (all frames) | 3.39 ms mean, 3.88 ms p99 |
| HEVC, AUD suffix, blocking wait, wait for parser | 0 (all frames) | 2.76 ms mean, 2.95 ms p99 |

### Chosen submit path

- `src/cedar_sync.c` exports its own `SemTimedWait` from the executable
  (`-Wl,--export-dynamic-symbol=SemTimedWait`). The dynamic linker binds the
  PLT calls in `libsbm.so` and `libcdc_base.so` to it. It blocks with
  `sem_clockwait` (`CLOCK_MONOTONIC`) and keeps the vendor contract: 0 when
  the semaphore was taken, -1 otherwise, -1 timeout means forever.
- Just before a vendor thread blocks, it advances a sequence number and wakes
  a futex. The parser blocks once per frame, right after it has consumed all
  submitted data, so the decode thread sleeps on that futex
  (`cedar_sync_wait`) instead of polling. Calls that can take the semaphore
  immediately (the parser's own message queue) do not count.
- Submit = frame + suffix in one `SubmitVideoStreamData`: the latest PPS seen
  in the stream for H.264 (refresh it from every IDR's parameter sets), an AUD
  for HEVC.
- Take the sequence number before submitting, wait until it changes (8 ms
  deadline), call `DecodeVideoStream`; on "no bitstream" before the deadline
  wait again; keep calling while an earlier submission is still undecoded.
- `TSP_SEMWAIT=vendor` forwards to the vendor function for A/B tests;
  `tsp-videotest` prints `semwait: interposed|vendor|NOT interposed` and the
  call counts so a run proves which path was active.

## Rotation

- **Direction.** A DE writeback capture (see "Methods") shows MainUI's
  top-left at the bottom-left of the 720x1280 scanout, so a 1280x720 picture
  must turn 90 degrees counter-clockwise: G2D flag `ROT_270` (`-G 270`).
  Checked with an asymmetric test pattern and with the frame marker, which
  lands at (32, 1247) in the rotated buffer (`-m 32,1247`).
- **VE rotation** (`VConfig.bRotationEn`/`nRotateDegree`): only the H.264
  plugin has a rotation path; `libawh265.so` has none. For H.264 the output is
  a correct 720x1280 picture, but the decode call grows from 3.3 ms to
  8.3 ms, which misses the 8 ms gate. Not used.
- **G2D rotate** (`G2D_CMD_BITBLT_H` 0x55 with `ROT_270`) into a ring of three
  720x1280 NV21 ION buffers (`src/g2d_rot.c`). The source is the decoded
  picture by IOVA (`use_phy_addr=1`). The ioctl returns once the hardware is
  done, so the source picture can be returned at once. About 1.0 ms per 720p
  frame with the one-time mapping below, 1.4-1.5 ms when the destination is
  passed as a dmabuf fd. The `sys_user` and `cma` heaps time the same; the
  ring uses `sys_user`.
- **G2D ABI.** The TrimUI kernel's `g2d_image_enh` is 108 bytes, not the 96
  of the public A133 BSP header. Disassembling `g2d_blit_h` and `g2d_ioctl`
  in the device's boot image gave `gamut` at 80, `fd` at 96, `use_phy_addr`
  at 100, the destination image at blit offset 112, and a 220-byte copy for
  `BITBLT_H`. `include/sunxi_g2d_abi.h` asserts this layout. The rotation
  flags are 0x100 (90), 0x200 (180), 0x300 (270), 0x400 (0); NV12 is format
  0x28, NV21 0x29.
- **ION** is the legacy 4.9 ABI (`ION_IOC_ALLOC`/`FREE`/`MAP` with handles,
  `struct ion_allocation_data` 32 bytes). Heap ids come from the device tree
  (`/proc/device-tree/ion/heap_*`): `sys_user` 0 (pages, IOMMU-mapped), `cma`
  4. The vendor demos `/usr/bin/g2d_*` were built for the newer ION ABI, so
  they are not a reference for allocation on this kernel.

## Display path

Tested with the rotated G2D ring on DE ch0/lyr0, z 32, NV21, BT601.

- **v2 layers import on every commit.** `disp_mgr_set_layer_config2` maps the
  dmabuf (`disp_dma_map`) on every `DISP_LAYER_SET_CONFIG2` and keeps the
  item in a per-manager list tagged with the vsync count. Items are freed only
  during later v2 commits, once older than the seven newest ids. Disabling
  the layer frees nothing, so the last two imports (2 x 1.38 MB here) stay
  pinned after exit until the next v2 session. The ION debugfs file lists
  them as orphaned buffers of the dead PID. The count is visible in the
  `dmabuf: cache[N] cache max[M]` line of `/sys/class/disp/disp/attr/sys`.
- **Flush.** `dispv_flush_imports` (`src/disp.c`) commits a 16x16 black,
  fully transparent dummy picture nine times, 20 ms apart, and then disables
  the layer. That pushes the real buffers out of the list (184 ms). Only the
  4 KiB dummy then stays pinned, and the next session releases it.
- **One-time IOMMU mapping through Cedar.** `/dev/cedar_dev` ioctl 0x502
  (`IOCTL_GET_IOMMU_ADDR`, `{fd, iommu_addr}`) maps a dmabuf and returns its
  IOVA; 0x503 (`IOCTL_FREE_IOMMU_ADDR`) unmaps the first entry with that fd
  and the caller's tgid, and returns 0 even if none matches. Mapping the G2D
  ring once (`g2drot_map_iova`, `-V`) instead of passing an fd on every blit
  cut decode+rotate CPU from 6.1% to 3.4% of one core (decode alone 3.3%) and
  the display test from 9.8% to 7.4%. Ring IOVAs were 0xfb200000,
  0xfb000000 and 0xfae00000.
- **Cedar mapping hazards** (from disassembling `compat_cedardev_ioctl` and
  `cedardev_release`):
  - The driver tags each mapping with the caller's tgid, and the release of
    *any* `/dev/cedar_dev` file of that process frees all of the tgid's
    mappings. Destroying the decoder (libVE closes its fd) therefore unmaps
    the ring as well. Keep a private Cedar fd for the process lifetime, call
    `g2drot_unmap_iova` before destroying the decoder and `g2drot_map_iova`
    after creating a new one; the IOVAs may be reused by then.
  - The ioctl that raises the engine reference count re-initialises the
    mapping list when the count reaches 1 (probably `IOCTL_ENGINE_REQ`).
    Entries added while no decoder holds the engine could be orphaned, so map
    only while a decoder exists.
- **Why not v1.** v1 layers (`-V -1`) show the ring by address with no
  import, but the DE holds no reference. If the process dies, the Cedar
  release unmaps the ring while the layer still scans it. The sunxi IOMMU
  fault handler logs every fault with `pr_err` without rate limiting and
  resets the faulting master, so that becomes a log storm until something
  disables the layer. With v2 the DE holds its own references: a dead engine
  leaves at most a frozen frame. v1 stays an experiment only.
- **Chosen:** v2 layer + G2D ring mapped once through Cedar for the blits;
  flush imports on every exit and in `--reset-display`.

## Display measurements

`tsp-videotest -d -G 270 -V -P <PPS> -W 8 -K -M -m 32,1247 test720p60.264`:

| Run | Shown | CPU (one core) | G2D mean / p99 | Layer ioctl | Submit to picture | Lag |
|---|---|---|---|---|---|---|
| fd per blit (before `-V`), 6 s | 362/362 | 9.8% | 1.40 / 1.50 ms | 0.60 ms | 4.85 ms | 0 |
| `-V`, 4 s | 242/242 | 7.4% | 0.98 ms | 0.62 ms | 4.59 ms | 0 |
| `-V -F`, 60 s | 3602/3602 | 7.3% | 1.01 / 2.29 ms | 0.63 ms | 4.49 ms (p99 7.44) | 0 |

- 60-second run: RSS 6232 -> 6340 KiB and flat after the first 5 s; ION
  total, CmaFree, the import cache count and the fd count did not change
  between the 10-second samples; afterwards ION was back to the baseline
  apart from the 4 KiB flush dummy. dmesg stayed clean.
- 10-minute headless decode+rotate loop (`-G 270 -V -t 600 -l 61`, no
  layer): 36002/36002 pictures, 0 errors, marker lag 0 for every frame,
  3.4% of one core, submit to picture 4.47 ms mean (p99 7.44, max 15.9),
  G2D 1.00 ms mean (p99 2.34). ION total, CmaFree and the fd count were
  identical in all ten per-minute samples and ION returned to the baseline
  afterwards. RSS rose 5932 -> 7000 KiB in 256 KiB steps; that is the test
  tool's own per-frame statistics arrays (28 bytes per frame, 984 KiB for
  36002 frames, written as the run proceeds), and the steps are the kernel's
  batched per-thread RSS counter (64 pages). The kernel has no
  `/proc/PID/smaps`.
- `kill -9` during display: the layer stays enabled with a frozen frame, two
  ring buffers stay pinned by the DE, and no IOMMU fault or other kernel
  message appears. `tsp-videotest -X -F -C 0 -L 0` then freed them, left only
  the fb layer enabled, and MainUI was intact (checked by capture).
- After every display test only ch1/lyr0 (the fb) was enabled and MainUI
  looked unchanged.

## Engine (`moonlight-tsp-stream`)

One binary: moonlight-common-c `e44e4290` (with enet and nanors) for the
stream, libgamestream from moonlight-embedded `33142e5` (`client.c`,
`http.c`, `mkcert.c`, `xml.c`) for HTTPS, pairing and launch. Sources:
`src/stream_main.c` (CLI, main loop, IPC), `src/video_cedar.c`,
`src/audio_alsa.c`, `src/input_evdev.c`, `src/wifi_ps.c`.

- **Commands:** `info`, `list`, `pair [PIN]`, `unpair`, `stream APP`, `quit`
  (all need `--host`), plus `reset-display` (also `--reset-display`) and
  `input-test`. `--help` lists the stream options (resolution, fps,
  bitrate, codec, audio latency/device, `--swap-abxy`, `--quit-after`,
  `--keep-wifi-ps`, `--duration`, `--stats`, `--ipc-fd`).
  `reset-display` also restores Wi-Fi power save after a killed engine (see
  "Wi-Fi power save").
- **Identity:** `--keydir` defaults to
  `/mnt/UDISK/trimui-tools/moonlight-tsp/keys`. If it has no
  `uniqueid.dat`, the stock identity in `/.cache/moonlight` (`client.pem`,
  `key.pem`, `client.p12`, `uniqueid.dat`, the same names libgamestream uses)
  is copied with mode 0600, `uniqueid.dat` last. The stock files are
  world-readable (0644); the copies are not. Nothing from them is printed.
  Tests so far used `--keydir /tmp/tsp-spike/keys`.
- **Video renderer: no `CAPABILITY_DIRECT_SUBMIT`.** common-c's decoder
  thread calls `submitDecodeUnit`, so the network receive thread never waits
  on Cedar or G2D. Its decode-unit queue holds 15 frames, and an overflow
  makes common-c request an IDR itself. (Earlier notes said DIRECT_SUBMIT;
  this replaces that.) Per frame: build the segments (H.264 appends the last
  PPS, HEVC an AUD), submit with pts = frame number x 16667, wait on the
  parser futex, decode, keep the newest picture, G2D-rotate 270 degrees into
  a 6-buffer ring (sys_user ION, mapped once through Cedar), and show it on
  DE ch0/lyr0 z 32 through a v2 layer. A buffer that left the screen is
  reused only after 34 ms (two vsyncs; no release fences on this BSP). Any
  decode error or damaged picture returns `DR_NEED_IDR`; a resolution change
  or 30 frames without a picture rebuilds the decoder.
  - The decoder labels each picture with the *previous* frame's PTS, so PTS
    cannot tell whether the current frame was decoded. The renderer counts
    instead: each decode result finishes the oldest submitted frame, and the
    loop stops when none is pending (capped at 2 so a dropped frame cannot
    wedge it). Every Sunshine frame decoded in one step.
  - The decoder is opened on the first decode unit, not at stream setup.
    Opened early, a vendor thread spins on `SemTimedWait(..., 0)` until data
    arrives: 1.5-1.8 million calls and nearly a full core during the first
    second. Opened lazily, the count at frame 120 is about 2.9 thousand.
  - The ring was 4 buffers. Two are on or just off the screen at any time,
    and Wi-Fi delivers frames in bunches, so 5.9% of pictures found no free
    buffer and were skipped (stream 3). With 6, 38 of 6339 were skipped
    (stream 4). `G2DROT_MAX_BUFS` is 8.
- **Audio:** Opus multistream to ALSA, also queued (no direct submit). The
  preferred PCM is defined in memory (`snd_pcm_open_lconf`): softvol on the
  system's "Soft Volume Master" control (so volume keys keep working)
  straight to `hw:audiocodec,0` with 5 ms periods, 20 ms start threshold,
  packets dropped while more than 40 ms is queued. While MainUI runs, it holds
  `hw:0` RUNNING through dmix (MMAP, S16_LE, 2 ch, 48000, period 1024,
  buffer 4096), so that open fails and the engine falls back to `default`
  (about 85 ms). Under the app launcher MainUI has exited, so the low-latency
  path should be free. Through dmix, starting with less than two slave
  periods underruns at once, so the start threshold is raised to 2048
  frames (buffer 4096, drop above 3072) on that path.
  - Sunshine sends 200 audio packets per second (5 ms Opus frames) and none
    during silence.
  - Audio is the largest CPU user: Opus decode 3.5% and ALSA (softvol, plug
    and dmix in the client) 4.7% of one core (stream 5). A fixed-point NEON
    libopus build and the direct hw path are the candidates to cut it.
- **Input:** evdev `TRIMUI Player1`. KEY bitmap `7cdb000000000000`: BTN_A,
  BTN_B, BTN_X (0x133), BTN_Y (0x134), TL, TR, SELECT, START, MODE, THUMBL,
  THUMBR; ABS `3003f`: X, Y, Z, RX, RY, RZ, HAT0X, HAT0Y. Mapping follows the
  xpad convention (BTN_A = Xbox A); `--swap-abxy` swaps A/B and X/Y.
  `input-test` showed the mapping is positional: the bottom button (labelled
  B) sends BTN_A = Xbox A, the right one (A) BTN_B, the top one (X) BTN_Y and
  the left one (Y) BTN_X. `--swap-abxy` therefore matches the printed
  labels. The hat reports HAT0X/HAT0Y as -1/0/1. The pad is announced to the
  host as an Xbox controller with analog triggers. The pad is not grabbed
  (keymon keeps its hotkeys; MainUI underneath also sees the presses). MENU
  is never forwarded: a tap pauses (CLI: toggles; with the UI: hides the
  layer, releases the pad on the host and sends `menu`). Start+Select+L1+R1
  held 1 s ends the stream. Both were tested live (stream 5). Holding MENU
  turns the pad into a mouse (see "Mouse mode").
- `input-test` absinfo: X/Y/RX/RY -32767..32767 and Z/RZ (triggers)
  0..255, all with `flat=0`, so the driver applies no dead zone; at rest
  the sticks sent no events in 3 s.
- Input devices: event0 sunxi-keyboard, event1 axp2202-pek (power key),
  event2 audio jack, event3 `TRIMUI Player1` (js0). `trimui_inputd`
  (`/mnt/SDCARD/trimui/app/trimui_inputd`) reads the controller MCU on
  ttyS3/ttyS4 and creates the pad through `/dev/uinput`.
- **Signals:** SIGINT/SIGTERM/SIGHUP/SIGQUIT go to one `sigwait` thread:
  before connecting they exit at once, while connecting they call
  `LiInterruptConnection`, while streaming they stop the main loop, and a
  second one while stopping disables the layer and exits. SEGV/BUS/ILL/FPE/
  ABRT disable the layer with one ioctl and re-raise. With `--ipc-fd` the
  engine sets `PR_SET_PDEATHSIG` so it cannot outlive the UI.
- **IPC (`--ipc-fd`, line based, non-blocking):** engine to UI
  `host PAIRED CURRENTGAME HEVC` (`info`), `app ID RUNNING NAME` (`list`),
  `pin NNNN` and `paired` (`pair`), `quit-done` (`quit`), and for `stream`
  `stage NAME`, `started`, `network poor|okay`, `menu`, `stats fps= lat=
  dec= rtt= kbps= lost= drop= cpu=` and `hb N` (both every second, always
  sent), `term CODE TEXT`, `ended`. Failures are `error unreachable`,
  `not-paired`, `pair-failed`, `other-app-running ID`, `launch-failed N`,
  `connect-failed N`, `stage-failed STAGE CODE` or `quit-failed`. UI to
  engine: `pause`, `resume` (also requests an IDR so a static screen
  repaints), `guide`, `stats on|off`, `mouse on|off`, `stop`, `quit-app`.
  Buttons still held when forwarding resumes are masked until released,
  and the MENU press that resumes from the UI does not reopen the menu on
  release.
- **Mouse mode** (`src/input_evdev.c`, 0.2.0): the pad drives the PC's
  mouse and keyboard instead of the virtual controller. It is on while
  MENU is held, or all the time after "Mouse mode: On" in the stream menu
  (UI) or `--mouse` (CLI). Layout, by printed label:

  | Input | Mouse mode |
  |---|---|
  | A, L1, R1 | left button |
  | B, L2, R2 | right button (triggers: pressed above 100, released below 60) |
  | X | middle button |
  | Y | Enter key |
  | d-pad | arrow keys |
  | right stick | pointer, up to 1800 px/s |
  | left stick | wheel, up to 2400 units/s (20 notches), both axes |
  | START, SELECT, L3, R3 | nothing (the exit combo still works) |

  The driver reports `flat=0`, so the engine applies its own dead zone of
  0.12: radial for the pointer, per axis for the wheel (so scrolling up
  does not also scroll sideways). The pointer speed follows u^2 (0.3 + 0.7u) of the deflection beyond the
  dead zone, so small moves are precise; the wheel follows u^2 and sends
  high-resolution scroll events (`LiSendHighResScrollEvent`, 120 = one
  notch) in chunks of at least 15. Motion is integrated on a 10 ms tick
  (the reader's `poll` timeout, only while a stick is deflected; gaps over
  50 ms count as one tick), with sub-pixel remainders carried over. Keys
  go through `LiSendKeyboardEvent` with Windows virtual key codes, 0x80 in
  the high byte as moonlight-embedded and moonlight-qt send them (Sunshine
  masks it off), and no modifiers.
- **MENU tap versus hold:** pressing MENU switches to mouse mode at once,
  so buttons pressed while it is held act as mouse buttons. The sticks act
  only after 200 ms, so a quick tap with a stick slightly off centre does
  not move the pointer. On release the stream menu opens only if the hold
  was shorter than 1 s and sent no mouse output; otherwise the pad returns
  to controller mode. With mouse mode switched on, MENU is no shift and a
  tap always opens the menu, even while the pointer moves. On every mode
  change the engine releases the mouse buttons, sends a neutral pad, and
  masks everything still held until it is released or back at rest:
  buttons, a stick beyond its dead zone and a trigger at 16 or more. So a
  button held across the change is not pressed in the new mode, and a
  stick still pushed when MENU is released does not turn the game's
  camera. All state lives under one mutex (`I.lock`) that the reader
  holds while it handles a batch of events; the engine's main loop takes
  it when it handles `pause`, `resume` and `mouse on|off` from the UI,
  then wakes the reader through its pipe.
- **Robustness of the input state:** the pad is opened with
  `EVIOCSCLOCKID` set to `CLOCK_MONOTONIC`, so event times compare with
  the engine's clock. The UI resumes the stream on a button press, and the
  reader might handle that press only after the engine has resumed; a
  press made before the resume is therefore masked (and a MENU press is
  no shift), whichever order the two threads run in. After `SYN_DROPPED`
  the reader reads keys, sticks, triggers and hat back with `EVIOCGKEY`
  and `EVIOCGABS`; a MENU release lost in the gap ends the hold without
  opening the menu. The exit combo also ends a MENU hold. If the pad
  disappears or fails, everything is released on the host until it is
  back.
- **Stats (`--stats`, every second):** received/shown fps, skipped, late,
  errors, IDR frames, `gap=MAX/N50/N100` (largest gap between the first
  packets of consecutive frames, and the count over 50 and 100 ms),
  first-packet-to-commit latency mean and max, assemble, queue, decode,
  rotate and commit times, host encoder latency, RTT, kbps, pending video
  frames, audio packets/lost/dropped/underruns/delay, and the process's own
  CPU from `/proc/self/stat`. Per-second fps hides sub-second stalls; the
  gap counts do not. At the end: a summary line, a `gaps:` line (also frames
  decoded while hidden, which are not "skipped"), the `semwait:` line
  (`interposed` and call counts), per-thread CPU from `/proc/self/task`, and
  `audio cpu: opus X%, alsa Y%`.
- **Wi-Fi power save:** on by default on this device (XRadio,
  `iw dev wlan0 get power_save`). It costs a lot during a stream (see the
  A/B below), so the engine turns it off just before connecting and back on
  right after the stream (and on a failed connect). `--keep-wifi-ps` leaves
  it alone; the UI has it as a setting, on by default. Before
  turning it off the engine creates `/tmp/moonlight-tsp-wifi-ps-off`;
  `wifi_ps_restore` (run after the stream and by `reset-display`) turns
  power save on only if that marker exists and then removes it. A killed
  engine therefore leaves power save off until the next `reset-display`,
  which the launcher always runs. A reboot clears `/tmp` and the driver
  starts with power save on. `iw` is run directly (`posix_spawn`, no shell).
  The battery cost of power save off is not measured yet.
- **Host facts (2026-11-14, `info`):** Sunshine at 192.168.0.40, HTTP 47989
  / HTTPS 47984, appversion `7.1.431.-1`, GFE version `3.23.0.74`, codec
  mask `0x180301` (H.264 and HEVC; no AV1), paired with the stock identity.
  It lists no display modes, so `gs_init` is called with `unsupported=true`
  (otherwise `gs_start_app` rejects every mode). Apps: Desktop and Steam Big
  Picture. At the check, the host reported Desktop as already running while
  this device was not streaming, so `stream` refuses a different app until it
  is quit (`quit`).
- **Sunshine stream facts:** the RTSP handshake reports "Reference frame
  invalidation is not supported by this host", so every loss costs an IDR.
  Data buffers start with the slice NAL (HEVC type 1 for P frames; IDR
  frames carry 32/33/34 VPS/SPS/PPS and 19), no AUD, split over many buffer
  entries. A static desktop is sent at about 21 fps and 230 kbps; video
  runs at 60 fps and 7-9 Mbit/s with the 10 Mbit/s setting. Termination
  code `0x80030023` is a graceful close by the host (seen when another
  client took over the session), reported as "host ended the stream".
  While this device streams, the user's other client cannot connect, and
  the reverse.

## Live stream results

All runs 1280x720@60, 10 Mbit/s, HEVC chosen automatically, app Desktop,
with MainUI running (so audio uses the dmix fallback). Display, ION and the
DE import cache were back at the baseline after every run unless noted.

| Run | What | Result |
|---|---|---|
| 1 | static desktop, 60 s | 1292/1294 shown (21 fps), 0 errors, lat 12.1 ms; every frame wrongly flagged late by PTS (fixed by counting) |
| 2 | decode trace | one decode step per frame, 2.7 ms; lat 4.5 ms (decode 2.7, rotate 1.0, commit 0.66); host closed with 0x80030023 at 48 s, clean exit |
| 3 | video, 90 s, 4-buffer ring | 4631/4920 shown, 289 skipped, lat 10.8 ms mean (max 156), 0 errors, 7.1 Mbit/s, 19.7% CPU; 203 unrecoverable frames from network loss |
| 4 | video, 120 s, 6-buffer ring | 6301/6339 shown, 38 skipped, 0 errors, 7.4 Mbit/s, 19.0% CPU |
| 5 | buttons | MENU paused and resumed, combo gave "stopped: exit combo" |
| 6 | `kill -9` at 30 s | layer stayed on (frozen frame), ION orphaned 4.15 MB, import cache 3; `reset-display` restored the baseline |
| 7 | host unreachable (iptables DROP from the PC for 40 s) | engine stopped by itself about 10 s later: "connection lost (-1)", exit code 2, clean state |
| 8-9 | Wi-Fi power save A/B | see below |
| 10 | engine-managed power save, `kill -9` at 62 s | power save off during the stream, marker left by the kill, `reset-display` restored power save and the display |

- **CPU** (stream 5, per thread, % of one core): audio decode thread 8.8,
  video decode 4.1, video receive 2.1, control 0.6, vendor decoder thread
  about 0.9. The whole engine is about 19% during video. The spike's decode
  and display path alone was about 7%, so most of the rest is audio.
- **Idle reference:** with only MainUI running, about 32% of one core is
  busy, MainUI alone about 21% in its menu (it redraws continuously).
  Other users are `trimui_inputd`, `keymon` and `trimui_scened`. Streams
  launched from MainUI will not have this, because MainUI exits for apps.
- **Wi-Fi stalls.** During streams the link stalled for 0.1-0.8 s about
  every 4 s, and less often for up to 1.8 s. Pings to the router and to the
  PC stalled together, so the cause is the TrimUI's radio; Bluetooth
  coexistence reported inactive. Idle 10 Hz pings never exceeded 36 ms.
  Power save A/B with 20 Hz pings to the router during a stream (the engine
  was not managing power save yet; it was switched by hand):

  | Run | Content | Power save | Router ping mean | max | over 50 ms | over 100 ms |
  |---|---|---|---|---|---|---|
  | 8 | static, 0.6 Mbit/s | off | 4.0 ms | | 3 | |
  | 8 | static, 0.6 Mbit/s | on | 8.5 ms | | 8 | |
  | 9 | video, 8.1 Mbit/s | on | 59.4 ms | 1774 ms | 61 | 32 |
  | 9 | video, 8.1 Mbit/s | off | 12.5 ms | 128 ms | 14 | 2 |
  | 10 | video, about 8.4 Mbit/s | off (engine) | 6.4 ms | 154 ms | 32 | 7 |

  Stream 10's frame gaps: largest 77 ms, 7 over 50 ms, none over 100 ms in
  61 s, with 55-63 fps in every second after the first.
- **OBSS scan stall.** A 1.7 s stall (5 fps in that second) at uptime
  311 s matched the access point's 300 s OBSS scan after the supplicant
  started at about 11 s. The Wi-Fi fix filters that scan only while its
  activation flag exists, and only the stock launchers create it, so the
  new launcher must join the fix's session contract.

## UI, launcher and package

### UI (`moonlight-tsp-ui`)

- One thread and one `poll` loop: signalfd (CHLD, TERM, INT, HUP), the pad,
  its key-repeat timerfd, the engine socket, the mDNS socket and an optional
  debug FIFO. It draws only after an input or a state change; while a stream
  is on screen it draws nothing and ignores the pad.
- Rendering uses the device's own SDL2 2.26.5 and SDL2_ttf from
  `/usr/trimui/lib` (video driver `mali`; `dummy` is also built in) and the
  stock font `/usr/trimui/res/regular.ttf`. The Makefile links against stub
  libraries generated from the UI's imports, so the binary binds to the
  device's unversioned libraries. All 36 SDL/TTF imports exist on the device.
- Input comes straight from evdev `TRIMUI Player1`, not from SDL: A open, B
  back, X settings, Y remove a PC or quit its running app, SELECT refresh,
  d-pad or left stick with repeat (380 ms delay, 110 ms rate). If the pad
  disappears (inputd restart) the UI retries every 2 s.
- Screens: saved PCs with online/paired/in-game status, search (mDNS
  `_nvstream._tcp` on the shared port 5353, see "Why the search missed two
  PCs"; each result is then checked with `info` in turn and
  shows paired, not paired or offline, and adding a checked PC keeps that
  status instead of checking again), add by IP (on-screen keypad), app list, pairing PIN,
  settings, connecting (stage names), the stream menu (Resume, Mouse mode
  on/off, Send Guide, Disconnect, Quit app and disconnect, with the mouse
  layout and exit combo in its footer) and dialogs for every error.
  Choosing "Mouse mode" sends `mouse on|off` and resumes the stream at
  once. It starts off in every stream. If another app is running on the PC, the UI asks before quitting it.
- Settings: 30/60 fps, 5/8/10/15/20/30 Mbit/s, codec Automatic/HEVC/H.264,
  swap A/B and X/Y, quit the PC app on disconnect, Wi-Fi power saving off
  while streaming (on by default), let the PC optimize game settings (off).
  Resolution is fixed at the native 1280x720.
- On-screen text follows ASD-STE100 Simplified Technical English (since
  2026-11-15): short sentences, simple tenses, active voice, no semicolons,
  "make sure that" instead of "check", "PC" (not "host") and "type" for
  PIN and address entry. Labels (titles, buttons, hints, badges, setting
  names) are names and stay short. The engine's stop reasons
  (`term_reason()`) are full sentences, because the "Disconnected" dialog
  shows them as they are, followed by "The error code is N."; `stream.log`
  prints them as `stopped: error N: TEXT`.
- Every host action is a separate engine run (`info`, `list`, `pair`,
  `quit`, `stream`) on a socketpair `--ipc-fd`, with timeouts (info 6 s,
  list 15 s, pair 300 s, quit 15 s, connect 60 s). The PIN screen names the
  PC and its address, because only that PC's Sunshine can accept the PIN. libgamestream's curl calls
  have no timeouts of their own, so these limits are what stops a hung
  request. During a stream a missing `hb` for 6 s gets the engine SIGKILLed;
  `stop` that is not obeyed within 10 s does too. Before either kill the UI
  writes `logs/hang.log` (see "First stream from the app"). After every stream the UI
  runs `moonlight-tsp-stream reset-display` (5 s limit).
- If the CrossMix CPU Performance scene is on (a process
  `com.trimui.cpuperformance.sh 1`, found in `/proc/*/cmdline`), the PC list
  header shows a warning instead of the version. The app never changes CPU
  settings itself (the user's choice).
- Data in `/mnt/UDISK/trimui-tools/moonlight-tsp/` (or `$MLTSP_DATA`):
  `config.ini`, `keys/` (the engine copies the stock client's identity from
  `/.cache/moonlight` on first use, so the PC needs no new PIN), and `logs/`
  (`ui.log`, `launcher.log`, `engine.log`, `stream.log`, each with a `.1`
  copy of the previous run, and `hang.log` from the last watchdog kill).

### Launcher (`package/launch.sh`)

- Writes `/tmp/stay_awake` (and removes it only if it created it), never
  touches the CPU governor or frequency limits.
- Joins the Moonlight Wi-Fi fix's session contract exactly like the fix's
  own wrapper: token `/tmp/moonlight-obss-sessions/$$`, flag
  `/tmp/moonlight-no-obss-scan`, and the 2.4 GHz frequency from
  `iw dev wlan0 link` in `/tmp/moonlight-obss-current-frequency`. The last
  session out removes the flag and the frequency file. With fix 0.2.1
  installed this activates the OBSS filter while the app runs.
- Sets `LD_LIBRARY_PATH=/usr/trimui/lib` and always runs `reset-display` on
  exit (also on HUP/INT/TERM), so a crashed UI cannot leave a video layer
  over MainUI or Wi-Fi power save off.
- Tested in the build container under dash and BusyBox ash with stand-in
  binaries: single run, a concurrent session left open (flag kept, only our
  token removed, a pre-existing `stay_awake` kept), and TERM during the UI
  (exit 143, cleanup ran).

### Package

- `scripts/package.sh` builds `dist/trimui-moonlight-tsp-<version>.zip`
  for extraction at the SD-card root (`Apps/MoonlightTSP/`): `config.json`
  (label "Moonlight HW"), a 300x300 `icon.png` rendered from
  `package/icon.svg`, `launch.sh`, both stripped binaries, `README.txt`,
  `LICENSE` (GPL-3.0), `licenses/` (moonlight-common-c, libgamestream, enet,
  nanors, OpenSSL, curl, expat, libuuid, opus) and `source/` (our tree plus
  the pinned third-party git sources; OpenSSL and curl by URL and SHA-256 in
  `third_party.lock`).
- The script fails if the engine stops exporting `SemTimedWait` or if
  either binary needs a library other than glibc, ALSA or the device's
  SDL2/SDL2_ttf, and it runs `make check`. Two runs give the same zip,
  and a clean checkout builds the same binaries (see "Build notes").
  0.2.0 (mouse mode, with the code review's input fixes) is
  `e47859339a164ad36df6d485555388ad6f9ff936ade9766272604d371c7526a5`,
  with UI `781d5bf1…` and engine `06a220b1…` (the first 0.2.0 build,
  `acf583bc…` with engine `e536507d…`, was installed but never launched).
  Both were installed on 2026-11-15 by stage-and-move while the 0.1.0 UI
  was running, and the 0.1.0 source archive was removed. Since the move
  to this repository the source archive also holds the docs, the README
  and `scripts/package.sh`, so the zip hash changes with every doc edit;
  the 0.2.0 binaries built here are the installed ones (UI `781d5bf1…`,
  engine `06a220b1…`). Do not record a zip's hash in a file that the zip
  contains. The last 0.1.0 build (pairing and search fixes, watchdog thread dump,
  descriptor guard) is
  `d3b70105f90cba49c2cbf7770eb482cf35c553251b8c3bb9b702a880af973466`
  (earlier builds were `66040522…`, `5ee0fe11…`, `298c6df4…`, `dfb3ef3b…`,
  `a9d63d88…` and `9f22333e…`). Its stripped UI is `09f6ef06…` and the
  engine `08f0b662…`.
- First installed 2026-11-15 by copying the zip to `/tmp`, checking its
  SHA-256 and running `unzip -o -d /mnt/SDCARD` (BusyBox `unzip` is
  `/usr/bin/unzip`). `LD_TRACE_LOADED_OBJECTS=1` confirmed every library
  resolves on the device.
- Updating while the app runs: BusyBox `unzip -o` would rewrite the running
  UI binary in place, which Linux refuses (ETXTBSY), leaving a half-updated
  folder. Instead, extract to `/mnt/SDCARD/.mltsp-stage` (same vfat
  filesystem), `mv -f` each file that differs (`cmp -s`) into
  `Apps/MoonlightTSP/`, then delete the stage. The running process keeps
  its replaced file (`/proc/PID/exe` shows `(deleted)`); the next launch
  uses the new one. The pairing and search updates and the watchdog dump
  replaced only `moonlight-tsp-ui` and the source archive; the engine was
  byte-identical.

### UI tests

- `scripts/ui-screens.sh` builds a host version of the UI,
  then runs `tests/ui-screens.sh` in the build container with SDL's `dummy`
  video driver and `tests/fake-engine.sh` (speaks the IPC protocol; 10.0.0.2
  is unreachable, 10.0.0.3 unpaired until paired). Keys and `shot PATH` go
  through `--debug-fifo`; `mdns ADDR NAME` adds a fake search result,
  because the container has no Sunshine to find. `FAKE_HANG=1` makes the
  fake engine go silent 2 s into a stream. The fake engine logs every
  command it receives to `ipc.log`; the script checks that choosing
  "Mouse mode" sends `mouse on` followed by `resume`, and the fake engine
  reports another MENU press 2 s later (shot 12b shows "Mouse mode: On").
  It saves 27 screenshots to
  `build/shots/` (PNG).
  `MLTSP_FONT_FILE=/tmp/tsp-libs/regular.ttf` uses a copy of the device font
  (not redistributed); without it DejaVu Sans is used.
- The README screenshots in `docs/screenshots/` are shots 05 (`pcs.png`),
  06 (`apps.png`), 12 (`stream-menu.png`) and 08 (`settings.png`) from a
  run with the device font, copied with
  `magick IN -strip -define png:compression-level=9 OUT` so the files hold
  only image data. Re-render them when those screens change.
- The 2026-11-15 run walked first start, saved PCs, apps, switch-app dialog,
  settings, connecting, streaming, the stream menu and disconnect, pairing,
  a dropped connection, a failed stage, removal, an unreachable PC, the
  performance warning, and search results being checked (paired, offline,
  not paired) and added without a second check. The logs showed the expected engine jobs and a
  `reset-display` after every stream. A later run added a silent engine:
  "heartbeat lost", a `hang.log` with its threads, the kill and the "Stream
  closed" dialog (shot 25).
- `tests/input-test.c` (`make check`, 80 checks) compiles
  `src/input_evdev.c` with stubbed `Li*` calls and feeds it events and
  times: pad mapping and A/B/X/Y swap, the MENU tap and hold rules, masking
  across mode changes, the mouse layout, trigger hysteresis, pointer speed,
  dead zone and scroll direction, pause and resume, and the exit combo in
  mouse mode. Later cases cover the code review's findings: a MENU tap with
  mouse mode on, a lost MENU release after `SYN_DROPPED`, MENU released
  after the exit combo, a partly pulled trigger and a pushed stick across a
  mode change, the resuming press read after the resume, and the pad
  disappearing.
- `tests/dump-test.c` (`make check`) runs the thread dump on a child blocked
  on a mutex and checks that it lists both threads, their syscalls and code
  addresses.

## First launch on the device

The user launched Moonlight HW from the Apps menu on 2026-11-15 (device clock
07:47; the clock is accurate but in CST, UTC+8).

- The launcher logged "Wi-Fi fix session on 2452 MHz". The UI started with
  SDL video driver `mali`, a 1280x720 display, renderer `opengles2` and font
  `/usr/trimui/res/regular.ttf`, and exited with 0.
- `ui.log` contains `sh: setterm: not found` during SDL start-up. Nothing in
  our source calls `setterm`; it most likely comes from the vendor SDL
  `mali` driver. It is harmless.
- The keys were copied from the stock client into `keys/` (mode 0700).
- An mDNS browse from the Mac found three Sunshine hosts on the LAN:
  `mac` (192.168.0.30), `gaming-pc` (192.168.0.20) and `htpc`
  (192.168.0.40, the PC the stock client is paired with). `serverinfo`
  reported Sunshine 7.1.431 on both .40 and .30.
- The app's search found only `mac`, the one PC it could find. With no
  saved PCs the app opened the search, and the user added `mac`. Its `info` reported not paired, so the app asked for pairing
  and showed a PIN. When the user entered the PIN, Sunshine said "No devices
  are waiting to pair". libgamestream's `/pair` request (device name
  `roth`) goes to one PC and waits there, so the PIN was most likely
  entered in another PC's Sunshine (`htpc`), or after the then 120 s limit
  had ended the request (`engine: job timeout` at 07:50:38).
- Fixes: search results show paired or not paired before a PC is added, so
  `htpc` stands out as already working with the copied identity; the PIN
  screen names the PC and its address; pairing waits 300 s.

### Why the search missed two PCs

The first search sent its query from an ephemeral port, expecting "legacy
unicast" answers (RFC 6762 6.7). Tested from the Mac with the same query
(Python, 2026-11-15):

| Query | mac (.30) | gaming-pc (.20) | htpc (.40) |
|---|---|---|---|
| ephemeral port, unicast answer | answers | silent | silent |
| port 5353, multicast answer (QM) | answers | answers | answers |
| port 5353, QU bit (unicast answer) | answers | not seen | not seen |

`gaming-pc` and `htpc` only answer standard queries from port 5353, which is
typical of Windows' built-in mDNS responder; Sunshine registers through it
on Windows. (The QU row is not conclusive: on the Mac, a unicast answer to
port 5353 can go to mDNSResponder's socket instead of the test's.)

The device runs `avahi-daemon` (it owns UDP 5353 and has joined
224.0.0.251 on wlan0; the stock client browses through it). Its config
keeps the default `disallow-other-stacks=no`, so other sockets may share the
port. The search now binds 5353 with `SO_REUSEADDR`, joins 224.0.0.251,
sends a QM query with ID 0 three times (0, 1 and 3 s), reads the multicast
answers and closes the socket after 4 s (it sees all mDNS traffic on the
LAN while open). If 5353 cannot be shared it falls back to the ephemeral
port, and `ui.log` says which mode it used. The parser now takes the
address from an A record of the SRV target, preferring the sender's own
address when a PC lists several (for example a Hyper-V adapter).

`tools/tsp-mdns.c` (`build/tsp-mdns`) runs the same browse from the command
line. On the device it found all three PCs: `htpc` and `gaming-pc` within 10 ms,
`mac` at 255 ms, in multicast mode, with avahi's socket untouched.
`tests/mdns-test.c` (`make check`, also run by the package build) covers
the parser: address choice, record order, foreign records and services,
every truncation of a valid answer, and a compression loop. It is also clean
under ASan and UBSan.

## First stream from the app (watchdog kill)

On 2026-11-15 at 08:17 (device clock) the user relaunched the app with the
search fix. The search found all three PCs in multicast mode. The user
picked `htpc` (192.168.0.40), and `list` and then `stream` of "Desktop"
(881448767) ran from the UI.

- `stream.log`: Wi-Fi power save off; "Reference frame invalidation is not
  supported by this host"; HEVC 1280x720@60 at 10 Mbit/s on ch0/0; audio on
  `hw:audiocodec,0` through softvol (48 kHz, period 240, buffer 1920, start
  960, drop above 1680 frames), so the low-latency path works under the real
  launcher; first audio after 400 ms, first video after 700 ms;
  `semwait: interposed calls=2937 blocked=354 timeouts=233` at frame 121.
- While it ran, the Wi-Fi fix status read `running_filter=loaded`,
  `wifi_state=COMPLETED`, with the activation flag, frequency 2452 and the
  launcher's session token present. The governor was `ondemand` at 600 MHz
  (performance scene off) and temperatures were about 35-45 °C.
- The user saw the stream. At 08:18:54 the UI logged "engine: heartbeat
  lost" and SIGKILLed the engine (08:18:55). The VE had been opened at
  uptime 5527.45 and the kill came at 5571.9, so video ran about 44 s; with
  the 6 s limit the last heartbeat was at most 37 s in.
- Nothing was logged after the `semwait:` line: no decoder rebuild, submit
  failure or stats error. Engine stdout is line-buffered, so no lines were
  lost to the kill.
- A passive snapshot from the Mac started at 08:18:53 (the device clock is
  within 1 s of the Mac's), after the heartbeat had already stopped, so its
  reads of `attr/sys` and the ION debugfs did not cause the hang.
- The kernel logged, during the killed engine's exit, `VE: release
  lost-lock...` and a `DEBUG_LOCKS_WARN_ON(lock->owner != current)` warning
  in `debug_mutex_unlock` from `cedardev_release` (thread `AudioDec`, the
  last one to drop the file). The engine therefore held the VE lock when it
  died: a thread was inside a hardware decode, or stuck there. The warning
  itself is only the debug-mutex check, because a different task released
  the lock. The kernel has `CONFIG_DEBUG_MUTEXES`.
- Recovery worked: `reset-display` ran (exit 0), only the fb layer was
  enabled, `dmabuf: cache[2]`, power save was back on and its marker file
  gone, the UI stayed up and listed the apps again. ION kept one 4096-byte
  buffer from the `reset-display` process, as after earlier resets.
- The heartbeat comes from the engine's main thread once a second, after
  `report()`. That path takes only short-held locks (video and audio stats,
  the IPC write lock), `LiGetEstimatedRttInfo` takes none, and the IPC
  socket is non-blocking. The one main-thread path that can wait on
  hardware is a MENU tap: `set_paused()` takes `disp_lock`, which the
  decoder thread holds during the G2D rotate and the layer commit. The
  cause is not known yet.

Since the evidence after a kill is thin, the UI now writes
`logs/hang.log` before any watchdog SIGKILL. For each engine thread it
records the name, state, `wchan`, `/proc/PID/task/TID/syscall` (for a
futex wait the first argument is the lock's address), up to 12 kernel
stack frames, the user PC, and the words on the user stack that point into
code, as `file+0xoffset`. The file ends with `/proc/PID/maps`. To read it,
run `addr2line -f -e build/moonlight-tsp-stream 0xOFFSET` for
`moonlight-tsp-stream+` entries (the build is reproducible, so the
unstripped binary matches the installed one), and map futex addresses
through the engine's `rw-p` mapping to symbols with `nm`.

## Second app stream (watchdog kill again)

The user relaunched the app with the thread dump and streamed "Desktop"
from `htpc` again. The UI started the engine at 08:38:32 and logged
"heartbeat lost" at 08:39:28, about 56 s later. The user saw nothing
unusual: no freeze before the stream ended.

- `stream.log` after the `semwait:` line: IDR requests, "Audio packet queue
  overflow" and "Network dropped audio data" (the audio thread had stopped
  being fed once the engine was stopped), then "wifi: power save restored",
  written by the UI's `reset-display`.
- `hang.log`, symbolized with `addr2line` in the build container: the main
  thread was in `cmd_stream`'s main-loop `poll(p, np, -1)` with 3
  descriptors (stream_main.c:791); the others were the evdev reader, the
  interposed `SemTimedWait`, common-c's queue and event waits
  (`LbqWaitForQueueElement`, `PltWaitForEvent`), the signal thread, the
  video decoder thread in `LiWaitForNextVideoFrame` and the audio decoder
  thread. No thread was in a lock wait or a VE ioctl: everything was idle.
- So the engine was not hung. The main loop waits on the timerfd, the wake
  pipe and the UI socket, and the heartbeat goes out on each timer tick.
  The UI resets its timer on any `hb` or `stats` line, and its read path
  looked correct. If the UI end of the socket had closed, the engine's
  write would fail with EPIPE (SIGPIPE is ignored), it would set
  `ipc_fd = -1`, and the poll would have had 2 descriptors, not 3.
- Suspected at the time: a stray `close()` somewhere in the process
  (a double close in vendor code) that hit the timerfd or the socket after
  its number had been reused. The third stream disproved this; the real
  cause was in the UI (see "Third app stream"). The guard below stays as
  cheap protection.

Changes for the next stream:

- `src/fdguard.c`: the engine exports its own `close()`
  (`-Wl,--export-dynamic-symbol=close`, like `SemTimedWait`), so every
  caller in the process, including dlopened vendor libraries, goes through
  it. The engine registers its long-lived descriptors (stdout, stderr, the
  UI socket, both ends of the wake pipe, the timerfd). A `close()` of one
  of them that does not come from the owner's `fdguard_close()` is refused
  (returns 0) and logged at most 8 times as `fdguard: refused close(N) of
  NAME from lib+0xOFF` with a backtrace (`dladdr` names exported
  symbols; vendor offsets need the device's own library copies). Other
  descriptors go straight to glibc's `__close`. Each second `report()`
  checks that every guarded descriptor still has its registered device and
  inode, and logs `fdguard: fd N (NAME) was replaced: now TARGET` once.
- `cmd_stream` prints `fdguard: interposed` (or `NOT interposed`) after the
  `stream:` line, and the summary counts refused calls.
- The main loop now polls with a 1 s timeout instead of none. It logs
  POLLNVAL once ("main loop: poll reports a closed fd"), and if the timer
  has not ticked for 1.5 s it logs "timer: no tick for N s" once and sends
  the report and heartbeat itself. A lost timer therefore no longer gets
  the engine killed.
- If a guarded descriptor is closed or replaced anyway, through a path that
  bypasses `close()` (glibc's internal closes such as `fclose`, `dup2`
  onto it, a raw syscall), `fdguard_ok()` reports it and the loop stops
  using it instead of spinning on POLLNVAL or reading another component's
  data: a lost timer logs "timer: fd N lost; ticking from the poll
  timeout" (poll timeout 500 ms, heartbeat about every 1.5 s), a lost wake
  pipe logs "wake pipe: fd N lost" (signals and MENU then wait for the
  timeout), and a lost UI socket logs "ipc: socket fd N lost" and ends the
  stream as "UI went away", before the watchdog would kill it.
- `ipc_send` logs write errors other than EAGAIN ("ipc: write failed …; UI
  link lost") and the first EAGAIN ("ipc: UI is not reading; dropping
  lines").
- The UI's watchdog line now reads "engine: heartbeat lost (last line N ms
  ago, N bytes received, socket open|EOF|error, fd N)". The UI closes its
  end on EOF or a read error after draining it, instead of spinning on
  POLLHUP.
- `hang.log` also lists each `ppoll` thread's descriptor set (`pollfd: fd/
  events …`) and, before `maps:`, every open descriptor with its target
  (`fds:`) and, for anonymous inodes such as the timerfd, its `fdinfo`.
- `tests/fdguard-test.c` (with `libfdguard-caller.so` for a caller in a
  shared library) and the extended `tests/dump-test.c` run in `make check`;
  both also pass on the device. The UI screen test's fake hang checks for
  the `fds:` section.

## Third app stream: the watchdog's clock bug

On 2026-11-15 the user relaunched the app with the descriptor guard and
streamed "Desktop" from `htpc` again (engine started 09:01:42). The stream
ran about 11 minutes; the watchdog killed it at 09:13:05 while the user was
launching Steam on the PC.

- `stream.log`: `fdguard: interposed`, and no `fdguard: refused`, `was
  replaced`, `timer:`, `main loop:` or `ipc:` line. No descriptor was
  closed behind the engine's back.
- `hang.log`: the main thread sat in `poll` on fds 4 (wake pipe), 34
  (timer) and 3 (UI socket). The timer's `fdinfo` showed `ticks: 0` with
  0.97 s left of its 1 s interval, so the loop had handled a tick about
  30 ms before. The video decoder thread was busy: its `syscall` line
  showed an `ioctl` on `/dev/cedar_dev` (0x102, the cedar driver's
  `IOCTL_WAIT_VE_DE` with a 1 s timeout) and its kernel stack, read a
  moment later, the G2D blit (`g2d_wait_cmd_finish`). Both are normal
  hardware waits, and dmesg had no VE errors. The engine was healthy.
- `ui.log`: "heartbeat lost (last line 18446744073709551615 ms ago, 52470
  bytes received, socket open, fd 16)". The last line was -1 ms old.
- Cause: the UI's main loop reads `now = now_ms()` once after `poll()`,
  then `drain_engine()` handles lines, and `on_engine_line()` stamps
  `J.last_hb = now_ms()` with a fresh reading. When the millisecond ticks
  over between the two readings, `last_hb` is `now + 1`, and the unsigned
  check `now - J.last_hb > T_HEARTBEAT` wraps to 2^64 - 1 and kills a
  healthy engine. The chance per heartbeat is small, which matches kills
  at 37 s, 56 s and 683 s. All three app streams were lost this way; the
  CLI streams never ran under the UI's watchdog.
- The first stream's `VE: release lost-lock` warning was a consequence:
  SIGKILL landed while the decoder thread was inside a VE wait.
- Fix: `ms_since(now, t)` returns 0 when `t` is later than `now`; the
  watchdog check and the three logged ages use it. A test-only
  `MLTSP_TEST_DRAIN_DELAY_US` makes the UI sleep between its clock reading
  and the drain. The new "late-lines" run in `tests/ui-screens.sh` streams
  for 5 s with a 2 ms delay: before the fix it logged "heartbeat lost
  (last line 18446744073709551613 ms ago, 151 bytes received …)" after the
  first heartbeat, after it none. The fake-hang run still kills after 6 s.
- Engine time arithmetic was checked for the same pattern: `cedar_sync`
  checks before it subtracts, the main loop subtracts older stamps from
  fresh ones, and the latency subtractions in `video_cedar.c` only feed
  statistics.

The same stream showed three short stalls, separate from the kill: each
time common-c logged "Audio packet queue overflow" (more than 30 packets,
150 ms, waiting), "Network dropped audio data" for the flushed packets,
then "Video decode unit queue overflow" and "Waiting for IDR frame" until
the requested IDR came. Sunshine's audio sequence numbers (200 per second,
from 0) date them at about 271 s, 571 s and 677 s into the stream; the
first two are exactly 300 s apart, the access point's OBSS scan interval.
Yet `/tmp/moonlight-obss-filter.log` did not exist: the filter had not
constrained a single scan since boot, although its activation flag and
frequency file (2452) were present and the filtered supplicant (pid 2462,
under the wrapper shell 2451) had the filter library mapped. A passive
`iw event -t` monitor then caught full-length scans (1.15-1.35 s) every
300 s, one of them inside a later stream. The filter recognizes only the
OBSS list for channel 10 (2457 MHz), and the access point is now on
channel 9 (2452 MHz), where the request is channels 2-12. Wi-Fi fix 0.2.2
matches the list for any channel; see the 2026-11-15 entry in
[moonlight-stutter-investigation.md](moonlight-stutter-investigation.md).

## Streams after the clock fix

The user relaunched the app (UI started 09:38:55, then again 09:44:19)
and streamed from `htpc` three times: 09:38:57-09:44:09 (about 5 minutes,
across the 09:41:14 OBSS scan), 09:44:22-09:44:33 and 09:44:36-09:45:06.
All three ended normally ("started 1 ended 1 term 0/0 killed 0"), with
no watchdog kill, and the user reported that it "seems to work great now".

The last two `stream.log` summaries were "9 s, 411 frames, 348 shown
(40.0 fps) … audio 0 lost 30 dropped 11 xruns, cpu 17.7%" and "28 s, 1555
frames, 1191 shown (42.0 fps), lat mean 7.3 max 64.9 ms, 39 skipped …
audio 0 lost 348 dropped 57 xruns, cpu 18.3%". The second ended "stopped:
by UI" and logged one "Audio packet queue overflow" with an IDR request.
Frames decoded while the stream menu hides the layer count as received
but not shown, which lowers "shown" fps for short streams that end
through the menu; the audio drops and xruns are not explained yet.

## Methods

- **Screen capture:** `echo /tmp/x.bmp > /sys/class/disp/disp/attr/capture_dump`
  writes the composed DE output (720x1280, 32-bit BMP, 3.7 MB) through the
  writeback unit. Copy it off and delete it from `/tmp`.
- **Kernel disassembly:** read the boot partition
  (`dd if=/dev/by-name/boot bs=1M count=32`, read-only), cut the kernel from
  the Android boot image (`ANDROID!` header, page-aligned), and disassemble
  with `objdump -D -b binary -m aarch64 --adjust-vma=0xffffff8008080000` in
  the build container. Function addresses come from `/proc/kallsyms` (root,
  not restricted). BSP sources for disp2, G2D and the IOMMU match closely
  enough to guide the reading, but struct layouts must be checked against
  the binary.
- **ION accounting:** `/sys/kernel/debug/ion/heaps/sys_user` lists buffers by
  PID, the total, and orphaned buffers (a dead PID's buffer still referenced
  by a driver).
- `scp` works (current OpenSSH `scp` uses the SFTP protocol); the
  `ssh trimui 'cat > /tmp/f.new && mv /tmp/f.new /tmp/f' < f` fallback also
  works.
- Harmless noise: CedarC prints `CdcIonFree ... errno:22` at decoder
  teardown, and `[BH_WRN] miss interrupt!` in dmesg is not from the kernel
  image (most likely the xradio Wi-Fi module).

## Build notes

- Docker image `moonlight-tsp-build:bullseye` (arm64, glibc 2.31, older than
  the device's 2.33). Bullseye security updates moved to archive.debian.org:
  the Dockerfile rewrites `debian-security` to that host because
  deb.debian.org still lists the packages but returns 404.
- The Mac has no `readelf`/`objdump`; run binutils inside that image.
- `make deps` (`scripts/build-deps.sh`) builds static OpenSSL 3.5.9 and curl
  8.23.0 into `build/deps` once (about 1.5 minutes). curl 8.18 and later
  require OpenSSL 3. `make` reruns the script when it changes, and the
  script rebuilds when its stamp (both versions and `SOURCE_DATE_EPOCH`)
  is missing. Before 2026-11-15 the Makefile only checked `.built`, so a
  version bump needed `build/deps` deleted by hand.
- Reproducible from a clean checkout (checked 2026-11-15): a fresh clone,
  a Docker image built with `--no-cache`, `fetch-third-party.sh` and
  `package.sh` gave exactly the installed 0.2.0 binaries (UI `781d5bf1…`,
  engine `06a220b1…`). Before that, only rebuilds in the same folder
  matched: OpenSSL writes its build time into libcrypto
  (`built on: Sat Nov 14 21:46:49 2026 UTC`), and a fresh `build/deps`
  got a new one, which also changed the build ID. `build-deps.sh` now sets
  `SOURCE_DATE_EPOCH=1794692809` (that original time), and the Dockerfile
  pins `debian:bullseye` to the digest 0.2.0 was built with. The zip can
  still differ between machines: `icon.png` is rendered by the host's
  ImageMagick (7.1.2-25 here). If Debian moves bullseye off
  deb.debian.org, point the Dockerfile's main sources at archive.debian.org
  too.
- Third-party sources compile with their own warning level and enet's
  configure results as `-D` flags; `HAVE_CLOCK_GETTIME` makes common-c use
  `CLOCK_MONOTONIC_RAW` instead of the adjustable wall clock (upstream's
  CMake never defines it, so upstream builds use `gettimeofday`).
- Linking: OpenSSL, curl, expat, uuid and opus static; glibc, libasound,
  libdl/libpthread/libm dynamic; CedarC dlopened. The binary needs glibc
  symbols up to 2.30.
- The device's `libasound.so.2` (alsa-lib 1.1.4.1) has no symbol versions,
  so a binary linked against Debian's versioned library makes ld.so print
  "no version information available" at every start. The Makefile links
  against a generated unversioned stub with the same soname instead; the
  device's plain names are the current (0.9.0rc4) hw_params API.
- The engine is 6.4 MB stripped (most of it libcrypto).

## Test tools

- `tsp-probe`: read-only check of the v1/v2 layer layout.
- `tsp-videotest [options] FILE.264|FILE.265`: decode-only unless `-d`.
  Useful flags: `-r FPS` pacing, `-t SECONDS`, `-l LOOPS`, `-M` marker lag
  (`-m X,Y` sample point; `32,1247` after `-G 270`), `-A` append AUD,
  `-P HEX` append bytes (e.g. the PPS), `-W MS` wait for the submitted frame,
  `-K` sleep on the parser instead of polling, `-G 90|270` G2D rotation,
  `-I sys|cma` ring heap, `-B N` ring size, `-V` map the ring once through
  Cedar, `-1` v1 layer by address (experimental), `-F` flush imports at exit,
  `-v` per picture, `-d` show on DE ch0 (takes over the screen),
  `-X [-F]` disable a layer (after flushing imports) and exit.
- The H.264 test stream's PPS suffix is `0000000168ef3cb0`.
- Current device copies live in `/tmp/tsp-spike/` (lost on reboot).

## Next

1. Moonlight Wi-Fi fix 0.2.2 (a separate project): once it is installed,
   check during a stream for a "constrained OBSS scan 2417-2467 MHz (11)
   to 2452 MHz" line in `/tmp/moonlight-obss-filter.log`.
2. Mouse mode on the device: after the review fixes the user reported
   that it "seems to be working great". Not yet checked: the
   `input: mouse mode on/off` log lines. Possible follow-ups: a pointer
   speed setting and a setting to start streams in mouse mode.
3. Explain the audio packet queue overflow and xruns in the short
   streams after the clock fix.
   Code review findings left open (2026-11-15): if the launcher is
   SIGKILLed its session token stays, so the OBSS activation flag and
   frequency file outlive the app until reboot (the Wi-Fi fix's own
   stock-launcher wrapper has the same design), and the frequency is
   written only at launch, so a channel change mid-session makes the
   filter fail open.
4. Under the real launcher, re-measure CPU (the low-latency audio path
   opens there). Note that the CPU Performance scene, if on, pins 2 GHz
   while the app runs.
5. Phase 5: passive Wi-Fi fix status while the app runs (flag and frequency
   files present, `running_filter=loaded`) and removed after it exits.
6. Measure the battery cost of power save off (TTE/charge counter A/B).
7. Audio CPU: fixed-point NEON libopus build.
8. Stock-client baseline stream (needs the user's go-ahead).
