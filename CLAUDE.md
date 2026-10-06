# Thanks Danny badge - notes for Claude Code

Firmware and tools for the "Sons of Sudo" badges: Waveshare ESP32-S3-Touch-AMOLED-1.75C
(466x466 round AMOLED, CST9217 touch, QMI8658 IMU, ES8311 audio). Three badges: Claudio, Danny,
Walter. Owner works on Windows; the badge is on COM13 (USB VID 303A).

Read first: `PROJECT.md` (what everything is, current version) and the newest sections of
`README.md` (engineering log, one section per version - add one for every change).
Private details (broker host, LAN IPs, what is set up where) are in `NOTES_local.md` - git-ignored.

## Build and flash

- Arduino core esp32 3.3.x, arduino-cli from the Arduino IDE install. FQBN (also in `Badge/sketch.yaml`):
  `esp32:esp32:esp32s3:FlashSize=32M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=app5M_little24M_32MB,FlashMode=qio`
- `tools\flash_badge.cmd`: clean build + upload + serial monitor (115200). The serial log is the main
  diagnostic tool; every subsystem logs with a prefix (`ui`, `touch`, `sos`, `vote`, `saver`, `sync`, `stall`...).
- Libraries: GFX Library for Arduino 1.6.x, SensorLib 0.5.x (`ImuDrv.hpp`, not the deprecated
  SensorQMI8658), TJpg_Decoder. MP3 = vendored Helix decoder in `Badge/src/helix`; MQTT = esp-mqtt
  (built into the core) wrapped in `Badge/src/sosnet.cpp`.
- After a change: build, check flash use (~56% now), bump the version string in `Badge.ino` setup(),
  update `PROJECT.md` + add a `README.md` section.

## Code layout and rules

- `Badge/*.ino` are concatenated alphabetically after `Badge.ino` (Decider, Gallery, Nope, Rumours,
  Screensaver, Sos, Vote). A file can use *functions* from later files (Arduino auto-prototypes them)
  but not *variables/macros* from later files. Keep function signatures to primitive types.
- Screens are `ST_*` in `Badge.ino`; `showScreen()` draws, `handleTap()` routes taps.
  Everything draws into a PSRAM canvas; use `flushRect()` for partial updates (even coordinates).
- The panel is ROUND: anything near the corners is invisible. Check positions against radius 233.
- Built-in font is 6x8 ASCII only (size 2 = 12 px per character). No UTF-8 on screen.
- Long work must never block `loop()` (it polls touch every 2 ms): network sync runs in a task on
  core 0 (`contentSyncStart()`), audio in core-0 tasks.

## Content and network

- Pictures / rumours / excuses come from the separate repo `github-pics/` (git submodule,
  n1ghtfly/danny-pics-7f3c9a), downloaded by the badges from raw.githubusercontent.com.
  Owner's workflow: files in `Downloads\ThanksDanny\pics` / `mp3`, then `update_badge_content.cmd`.
- Badge network: MQTT on the owner's own Mosquitto (IOTstack on a Pi, TLS via Nginx Proxy Manager
  stream on 8883; WebSockets on 9001 behind an NPM proxy host for `web/poll.html`). Topics and ACL:
  `server/mosquitto/` - every topic that names a sender must be enforced with `%u` in the ACL.
  `tools/sos_cli.py` (watch / who / slap) for testing from a PC.

## Never

- Never put the broker address, IPs, SSIDs, MAC addresses or any password in a tracked file:
  the repo is PUBLIC. Defaults go in `Badge/sos_local.h` (git-ignored). Passwords are only typed
  on the badge's phone setup page and are never logged.
- Don't commit build output (`build/`, `*.bin`) or the third-party wallpapers (`wp*.jpg`).
