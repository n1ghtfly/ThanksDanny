# Thanks Danny — Waveshare ESP32-S3-Touch-AMOLED-1.75C

> **Start with [PROJECT.md](PROJECT.md)** — what the badge does, the folder map, the build
> setup and how to flash. This file is the engineering log: hardware facts and the reasoning and
> measurements behind every version.

A watch-style AMOLED badge board. This is the project folder for it; the sketch lives in
`Bringup/` for now.

## Board — verified on the hardware, not from a listing

| | |
|---|---|
| MCU | **ESP32-S3** (QFN56) rev **v0.2**, 240 MHz, dual core + LP core |
| PSRAM | **8 MB** embedded octal (detected by the ROM: AP, generation 3, 80 MHz) |
| Flash | **32 MB** (manufacturer `c8` GigaDevice, device `4019`), quad, 3.3 V |
| USB | **USB-Serial/JTAG** — auto-reset works, no BOOT-hold needed |
| Port | **COM13** |
| Panel | **CO5300**, 466×466 AMOLED, QSPI — **no backlight pin**, brightness is panel command `0x51` |
| Touch | **CST9217** @ I²C `0x5A`, both axes mirrored |
| PMU | **AXP2101** |
| IMU | **QMI8658** |
| Audio | **codec, not a bare DAC**: ES8311 output + ES7210 dual-mic ADC, amp enable on GPIO 46 |
| Buttons | PWR and BOOT on the sides |

## Pin map — Waveshare's own `pin_config.h`

Taken from `examples/arduino/libraries/Mylibrary/pin_config.h` in the vendor repository
(their file, their numbers):

```
Display   LCD_SDIO0 4   LCD_SDIO1 5   LCD_SDIO2 6   LCD_SDIO3 7
          LCD_SCLK 38   LCD_CS 12     LCD_RESET 1   466 x 466
Touch     IIC_SDA 15    IIC_SCL 14    TP_INT 11     TP_RST 2
Audio     ES7210 BCLK 9  LRCK 45  DIN 10  MCLK 16
          ES8311 DOUT 8  PA enable 46
```

**The 6-column offset matters.** The 466-wide panel sits 6 columns into the controller's
480-wide RAM, so the panel object is constructed as:

```cpp
Arduino_DataBus *bus = new Arduino_ESP32QSPI(12, 38, 4, 5, 6, 7);   // CS, SCK, D0..D3
Arduino_CO5300  *gfx = new Arduino_CO5300(bus, 1, 0, 466, 466, 6, 0, 0, 0);
//                                          RST  rot  W    H   ^ col_offset1 = 6
```

`gfx` must be typed `Arduino_CO5300`, not `Arduino_GFX` — `setBrightness()` is the
panel's own member and the base class has no such call.

## Build & flash

```
--fqbn esp32:esp32:esp32s3:FlashSize=32M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=huge_app,FlashMode=qio
```

Note **FlashSize=32M** — this board is not the 16 MB one from the cyberpunk project.
Bring-up build: 386,544 bytes (12% of 3 MB), globals 7%.

Factory firmware as shipped: **`esp-brookesia`** (Espressif's UI framework, app version
`ac40993`, compiled Jan 14 2026) — the badge demo it boots with.

## Factory backup

`backups/AMOLED175C_factory_32MB.bin` — full 32 MB read of the chip as shipped, before
anything was flashed over it, plus a `.sha256` beside it. Waveshare also publishes factory
recovery binaries in their repo `Firmware/` folder, so there are two ways back.

## Libraries

| Library | Why |
|---|---|
| `GFX_Library_for_Arduino` 1.6.8 (installed) | has `Arduino_CO5300` already |
| `SensorLib` 0.5.0 (installed) | CST9217 touch + QMI8658 IMU |
| `XPowersLib` | AXP2101 PMU — install when battery/rail readings are needed |

The vendor bundles **its own** copy of `GFX_Library_for_Arduino` under
`examples/arduino/libraries/`. If the installed 1.6.8 ever fails to initialise the panel,
that bundled copy is the next thing to try — not a reason to start changing wiring.

## Bring-up sketch — what it proves

`Bringup/Bringup.ino` does three things in order and prints what it found:

1. Reports chip / flash / PSRAM / heap, and parks the PA enable line low.
2. Sweeps I²C on SDA 15 / SCL 14 and names what it recognises (`0x34` AXP2101,
   `0x5A` CST9217, `0x6A`/`0x6B` QMI8658) — so the bus and the chips are confirmed
   by response rather than assumed from a spec sheet.
3. Brings up the CO5300, sets brightness, and draws colour bars plus a title. Wrong
   colour order or a wrong column offset would be visible immediately.

## Wallpapers on the device (done)

The user's `Downloads\ThanksDanny\wallpaper` folder (6 cyberpunk JPEGs, 5.3 MB) now lives in
the board's own flash. The badge displays **the Sons of Sudo picture, held permanently** —
"only 1 wallpaper: the one with Sons of Sudo" — and the other five stay on the device unused,
so switching to any of them (or to a pair) is a one-line edit of `SHOW` in the sketch. Nothing
to plug in, nothing to lose.

- **Layout**: `PartitionScheme=app5M_little24M_32MB` — 4.8 MB app (sketch uses 437 kB, 9%),
  filesystem partition `spiffs` at **0x910000**, 23,986,176 bytes (23,424 kB reported by
  `LittleFS.totalBytes()`), 424 kB used.
- **On the device**: `/wallpaper/*.jpg`, all **466x466 baseline**, 27-89 kB each.
- **Firmware**: `Wallpaper\Wallpaper.ino` — CO5300 + LittleFS + TJpg_Decoder from a `File`
  handle, list + sort the folder, one picture full screen, advance every 10 s.
- **Tools** (re-runnable when the picture folder changes):
  - `tools\prep_wallpapers.py` — crops to 466x466 with a per-image vertical bias and forces
    baseline JPEG (3 of the 6 originals were progressive, which TJpg_Decoder cannot read).
    Verified by reading the files back off disk, not by trusting the save call.
  - `tools\make_fs_image.py` — reads the offset and size **out of the partition CSV**, builds
    the image with mklittlefs, then unpacks it and compares every file byte-for-byte.
  - Flash the image: `esptool -p COM13 -c auto write-flash 0x910000 build\wallpaper.littlefs.bin`
    (~3 min; the app uploads separately via arduino-cli under the same scheme).
- **Measured**: each wallpaper decodes and draws in **297-374 ms** (900 blocks of 16x16).
- **Watch item**: the first boot after flashing the filesystem failed to decode all six
  (result 2), then worked on every boot after. Cause not proven; hardened with one file
  handle per draw plus explicit closes, and 6/6 now draw across a full cycle.

## Badge app — home screen, cog, settings, Wi-Fi (current firmware)

**The panel is ROUND.** 466x466 is the bounding square; the visible area is a circle of radius
233, so anything drawn near a corner draws fine, logs fine, and is masked off the glass. The
first version of this sketch put the cog at (400,46) and the status pill at (14,418) — both
invisible, which is exactly what the user reported. All screen furniture now sits inside the
circle and a boot-time layout check proves it:

```
panel  : CO5300 up, 466x466 round (visible circle r=233)
layout : cog on glass yes (centre 175 + disc 33 of 229) | status pill yes | menu rows yes
```

Run the check before adding any new control; a render through a circular mask
(`preview_home_round.png`) is the fast way to see what the user will actually get.

`Badge\Badge.ino` + `Badge\Gallery.ino` are the device's main firmware (1,272,743 bytes, 26% of
the app partition). Arduino concatenates the two `.ino` files into a single translation unit,
and that matters: **`Badge.ino` is compiled first**, so anything it references must be declared
above it — a `#define` living in `Gallery.ino` is invisible to `Badge.ino` and the compiler says
so plainly. Verified at boot: `touch : CST9217 at 0x5A`, `touch : range 466x466, mirrorX 1
mirrorY 1`, `wifi : connected to '<your network>', ip <its address>`, `time : asked SNTP for the
correct time`, `bg : son_of_sudo.jpg result 0 (ok), 900 block(s), ~400 ms`.

- **Home**: the Sons of Sudo wallpaper, a cog wheel button **inside the circle** upper right
  (355,108), and a status pill **centred at the bottom** (240x44 at y=380) showing Wi-Fi and
  touch state.
- **Cog -> Settings**: four rows — "Pictures", "Wi-Fi setup on phone", "Touch test", "Close".
- **Wi-Fi setup**: the badge raises its **own open access point** (`ThanksDanny-setup`,
  `http://192.168.4.1`) and serves a page listing the scanned networks. The password is typed
  **on the phone** — never displayed on the badge, never written to the serial log (only the
  SSID and the password's character count are logged, so a pasted log cannot leak it).
- **Credential rule**: the user must never paste the password into a chat, and this design
  means they never need to. Do not add on-screen password display or password logging later.
- **Failure handling**: the radio runs **AP+STA**, so a wrong password does not take the setup
  page away — it stays up and they can retry. The AP is torn down only on success.
- **Scan caching**: the SSID list is scanned once at boot and cached, because scanning in pure
  AP mode drops the phone that is loading the page — and because a scan measured **3363 ms**
  on the touch path, which read as an unresponsive button. The setup page carries a
  **`/rescan`** link, so a network that appeared since boot is picked up on demand: the delay
  lives on the phone, where it is expected, instead of on the badge's tap path.
- **Measured responsiveness** (400 kHz bus, 2 ms poll, 90 ms tap guard): one touch read
  **0.75 ms**; cog → Settings **119 ms**; Close → wallpaper **452 ms**; a tap that hits no
  control **0 ms**. The panel fills at only **~0.6 Mpixel/s**, so a full-screen repaint is
  ~350 ms and the wallpaper ~390 ms. That is the floor, and it is why every tap draws a small
  acknowledgement (a ring round the cog, an outline round the row) before any slow work.
- **Touch init detail**: `touch.sleep()` must NOT be called — with it the controller reports
  zero points forever, which looks exactly like a dead panel. `reset()` alone is correct.
- **Touch test screen**: four target circles; taps are drawn where they land, so a mirroring
  problem is visible on the panel rather than only in a log.
- Credentials are stored in NVS (`Preferences`, namespace `wifi`).

## Picture gallery — GitHub-hosted pictures on the panel (done)

A "Pictures" app downloads JPEGs from a GitHub Pages site and scrolls through them with a slide
transition, ~13 s per picture, in random order.

- **Pipeline**: `Badge\Gallery.ino` fetches `list.txt` + the JPEGs over **HTTPS into LittleFS**,
  then decodes **from the file handle** into a PSRAM frame. `TJpg_Decoder` has no network-stream
  entry point (checked in its source rather than assumed), and caching first also means the
  slideshow keeps working when the Wi-Fi drops.
- **`list.txt`, not JSON**: the badge has no JSON parser and adding one costs flash for nothing.
  One `<name> <bytes> <width> <height>` line per picture. The width matters — it is the decode
  **stride**, and the app must display with the same stride or the picture shears. Entries larger
  than the frame (`1100 x 466`) are skipped rather than trusted.
- **TLS needs a clock — this cost the entire first attempt.** The badge has no RTC, so it boots
  at 1970 and every certificate in the CA bundle then looks *not yet valid*: the handshake fails
  and it presents as "the download simply never happens". `configTime()` (SNTP) on connect fixed
  it, and the boot log proves it — `gallery: heap 218 kB, clock 243` → `clock is now
  1790979158`. With a sane clock the fetch is **properly validated** (`tls validated`); no
  `setInsecure` is needed.
- **Write the body on every success path.** The first version wrote the file only inside the
  insecure-fallback branch, so the validated path fetched 113 kB, threw it away, and still
  reported **HTTP 200**. The serial log — not the screen — is what exposed it: `HTTP 200, 0 of
  113181 bytes`.
- **Boot fetch + boot self-test** (`galleryBootFetch()`, `galleryWarmup()`): on connecting, the
  badge syncs the list and any changed pictures by itself, then decodes two cached pictures and
  runs eight slide frames. So the download *and* the visual path are exercised at every boot
  without anyone tapping, which is how both were finally verified.
- **Measured**: decode **233-389 ms** per picture; slide frame **77 ms** (13 fps); the deck
  cycles landscapes (835x466) and portraits (466x695, 466x619) alike; **5,729 kB PSRAM free**
  with all three buffers allocated; heap flat at **218 kB** across all six downloads.
- **Hosting**: `https://n1ghtfly.github.io/danny-pics-7f3c9a/` — public (required for free
  Pages), unguessable name, `noindex,nofollow`. Prepared from `Downloads\ThanksDanny\pics` by
  `tools\prep_github_pics.py` (835x466, **baseline** JPEG — progressive fails silently in
  `TJpg_Decoder`), and verified by re-reading every served file and comparing sha256.
- **Adding pictures**: drop files into `Downloads\ThanksDanny\pics`, run
  `tools\prep_github_pics.py`, push. The badge picks them up on its next boot or app entry — no
  reflash needed.

## v2 — faster screens, still pictures (Oct 3)

Two complaints: "a bit sluggish", and "the picture app shows the pictures but then they scroll".

- **Off-screen canvas.** `gfx` is now an `Arduino_Canvas` (466x466, PSRAM) in front of the panel,
  which is `panel`. Screens are composed in RAM and sent with **one** `gfx->flush()`. Drawing
  primitives straight at the panel was the slow part - `fillScreen` alone was ~350 ms, and every
  screen change began with one (the home screen painted black, then covered it with the
  wallpaper). Rule for new code: draw on `gfx`, then flush; nothing shows until the flush.
  `setBrightness()` is called on `panel`.
- **QSPI clock 80 MHz** (`LCD_QSPI_HZ`, was the library default 40 MHz). The boot log prints the
  measured full-frame time (`panel : ... QSPI 80 MHz, full frame N ms`). **If the screen shows
  noise or stripes, set `LCD_QSPI_HZ` back to 40000000** - nothing else depends on it.
- **Gallery shows pictures still.** No more 13-second pan: each picture sits centred (a third
  of the way down for tall ones) for 10 s, the caption disappears after 2.5 s, then a 12-frame
  slide brings in the next. Tap = next picture, cog = settings, BOOT = home.
- **One tap = one picture.** Taps in the gallery are taken on the press edge; before, a finger
  still down after a change counted again and skipped pictures.
- **Offline pictures no longer shear.** The decode stride is read from the JPEG header
  (`getFsJpgSize`) instead of trusting list.txt or the 466x466 offline guess.
- **The gallery no longer stops after 200 pictures** (it used to return and leave a frozen
  screen).
- Window/slide code tested off-device under AddressSanitizer for 835x466, 466x695, 466x619,
  466x466, 1100x466 and smaller-than-panel pictures: no out-of-bounds reads, centre pixels right.
- Build (esp32 core 3.3.12, GFX 1.6.8, SensorLib 0.5.0, TJpg_Decoder 1.1.0): **1,284,039 bytes**
  (27%), globals 17%. Not yet run on the hardware at the time of writing.
- **v2 fix (same day): the picture app showed nothing.** TJpg_Decoder's `getFsJpgSize()` and
  `drawFsJpg()` both **close** the `File` they are handed. The new header-size check reused that
  closed handle for the decode, so every picture failed. `decodeToFrame()` now probes with one
  handle and decodes from a freshly opened second one. All three `drawFsJpg` call sites open their
  own handle. Build 1,284,283 bytes.
- v1 sources kept in `backups/Badge_v1/`.

## Decider app (v2.2)

A gold **coin button upper left** on the home screen (the cog's mirror image, same size and
distance from the centre, so the boot layout check covers it: `layout : ... decider yes`).

- **Tap it**: a coin is tossed - it rises, flips end over end (the face squashes to an edge and
  back), grows as it comes closer, its floor shadow shrinks, then falls while the spin slows and
  lands flat. The landed coin zooms up into the full-screen badge: **Approved by Danny** or
  **Disapproved by Danny**, 50/50 from `esp_random()`, decided before the throw - the spin is
  planned in whole turns (approved) or whole turns plus a half (disapproved).
- After 1.2 s a pill appears at the bottom: **AGAIN | HOME**. A tap anywhere except HOME tosses
  again; BOOT goes home from anywhere, mid-throw included.
- The throw height was checked in a render through the round mask: a first peak of 200 px put
  the coin's top past the glass, so it is 120 (centre y~155, radius ~102 at the top).
- **Assets compiled in**, not on LittleFS: `Badge\decider_assets.h` (raw RGB565: two 466x466
  results + two 200x200 coin faces, ~1 MB of flash). No JPEG decode at run time and no
  filesystem reflash, so the cached GitHub pictures stay. Regenerate with
  `tools\prep_decider.py` after changing `Downloads\ThanksDanny\chooser\positive.png` /
  `negative.jpg` - it crops each badge to its navy outer ring (the checkerboard in the corners
  is painted into the images, not transparency) and blacks out everything outside the circle.
- Frames are composed in the canvas; each toss frame restores only the coin's previous
  bounding box from the backdrop instead of copying all 434 kB.
- Build: **2,316,363 bytes** (49% of the app partition).

## Home carousel (v2.3)

The three corner buttons are gone from the home screen. Instead: **one big icon in the middle**
(radius 60, was 42), its neighbours smaller at the sides (36) so it is obvious there is more,
page dots, and a pill with the app's name and the Wi-Fi line. Apps: **Pictures, Decider,
Settings** (Settings = the old cog menu). `preview_home_carousel.png` shows it settled,
mid-swipe and on the next app, rendered through the round mask.

- **Swipe** left/right to move (wraps round). A tap on the big icon opens it; a tap on a side
  icon slides that app to the middle. BOOT still goes home from anywhere.
- **Touch is now gesture-based** (`pollTouch()`): a press becomes a swipe the moment it has
  moved 45 px sideways (and more sideways than vertical) - it fires mid-gesture so the
  carousel moves under the finger. A press released without becoming a swipe is a tap, at the
  point where it went down. Taps therefore fire on **release** now (they used to fire on press).
  Release needs 3 zero-point reads in a row (~6 ms), so one dropped report mid-drag cannot turn
  a swipe into two taps. A drag of more than 30 px that never became a swipe is ignored.
- The slide is 5 frames with ease-out; each restores only the wallpaper band y=214..424 from the
  PSRAM copy (`wallpaperRows()`) instead of the whole screen.
- The home screen is redrawn when Wi-Fi connects, so its Wi-Fi line shows the address.
- The gallery keeps its own cog (top right) for settings; the boot layout check now covers the
  three carousel slots and both pills: `layout : app carousel yes | gallery cog yes | ...`.
- Build: **2,317,555 bytes** (49%).

## Decider sound (v2.4)

A **drum roll while the coin is in the air, a cymbal crash on the frame it lands**.

- `Downloads\ThanksDanny\chooser\drummroll.mp3` is 8.8 s: roll to ~5.1 s, crash at 5.13 s
  (found from the loudness, not guessed), silence after. `tools\prep_decider_sound.py` cuts the
  roll to the toss length (2.4 s), crossfades 40 ms into the original crash, keeps 1.7 s of it,
  fades the tail and normalises: **4.08 s, 16 kHz mono, 128 kB** in `Badge\decider_sound.h`.
  `drumroll_as_on_badge.wav` is exactly that cut, for listening on the PC. Re-running the
  script needs ffmpeg on PATH.
- The toss is now **timed** (`DEC_TOSS_MS` = 2400) instead of 38 counted frames, so the crash
  lands with the coin at any frame rate. `DEC_TOSS_MS` and the script's `TOSS_MS` must match.
- Audio path as proven by `AudioProbe`: I2S (MCK 16, BCK 9, WS 45, DO 8, DI 10) -> ES8311 @ 0x18 on
  the touch I2C bus -> amp enable GPIO 46. The ES8311 driver files are copied from AudioProbe
  unchanged. I2S/codec start on the first toss; if any step fails the Decider runs silently and
  the log says which step.
- Samples are streamed by a FreeRTOS task on **core 0**, so the animation (core 1) never waits.
  The amp is switched off after each play (no idle hiss); a new toss or BOOT cuts the sound off.
- Codec volume `AUD_VOLUME` = 80 in `Decider.ino`.
- Build: **2,479,987 bytes** (52%).

## v2.5 - Decider icon

The Decider's carousel icon is a **thumbs up** (was a gold coin with "?"), drawn from rounded
rectangles in the same light-blue style as the Pictures and Settings glyphs, scaled to the slot:
`drawDeciderIcon(cx, cy, r, fg, bg)` in `Decider.ino`. Build 2,480,399 bytes.

## v2.6 - touch responsiveness

"A bit sluggish reacting to touches." Not the wallpaper: it is a 20 ms copy, and a whole screen
change measured ~70-80 ms in the v2.x logs. Two real causes:

1. **v2.3 made every tap wait for the finger to lift** (needed on the carousel, where a press
   may become a swipe) - on every screen. Now only the home carousel waits for the lift, and
   it **lights the icon up the instant the finger lands**. Every other screen (settings, Wi-Fi,
   touch test, Decider result) taps **on press** again.
2. **Lift detection was count-based and the touch driver lies between reports.** SensorLib's
   CST92xx `getTouchPoints()` returns zero points whenever a read lands between two controller
   reports (the chip answers ACK when it has nothing new). At a 2 ms poll a finger still down
   reads "no finger" several times running, and 3 empty reads counted as a lift. Now a lift is
   **35 ms with no report** (`RELEASE_MS`). Simulated with 2 of every 3 polls empty: one tap per
   press, swipes fire mid-gesture, a held menu press does not repeat.

Plus **partial flushes**: `flushRect(x, y, w, h)` sends only part of the canvas (CO5300 window
widened to even columns/rows). The press highlight is a ~140 px patch (~5 ms instead of 46),
the carousel slide sends only its band (y 214-424), and each coin-toss frame sends only the old
patch plus the new coin, so the throw runs at a higher frame rate. Build 2,481,327 bytes.

## v2.7 - no cog in the picture app

The cog that sat over every picture is gone; nothing is drawn over the pictures now. Controls:
**tap = next picture, hold 0.7 s = main screen, BOOT = main screen**. The first picture's
caption says so ("TAP NEXT  HOLD HOME") for 2.5 s. The gallery's tap detection now uses the same
time-based lift as the home screen (35 ms without a report), so a long press cannot skip several
pictures. Build 2,481,179 bytes.

## v2.8 - Rumours app

A fourth carousel app (Pictures, Decider, **Rumours**, Settings) with a megaphone icon. It plays a
random rumour - a short MP3 about Danny - with the speaker's name, an "AI voice parody" line
under it, and sound bars that follow the real loudness of each decoded frame. AGAIN | HOME at
the bottom like the Decider; tap anywhere else for another one; leaving the screen stops it.
`preview_rumours.png` is a render through the round mask.

- **Source**: `Downloads\ThanksDanny\mp3` - 9 AI voice parody clips (44.1 kHz, 128 kbit/s,
  4-35 s). `tools\prep_rumours.py` re-encodes them to 16 kHz mono 48 kbit/s with even loudness
  (3.3 MB -> 1.2 MB), names them `rNN.mp3`, and writes `rumours.txt`
  (`<file> <bytes> <speaker>`) into `github-pics\rumours\`. Speaker names come from the
  `[brackets]` in the file names, tidied by a table in the script.
- **Hosting**: the public GitHub Pages site, next to the pictures (the user chose public over
  badge-only). `push_pictures_site.cmd` publishes it. The site README and `rumours.txt` say the
  clips are AI parody, and so does the badge screen.
- **Download**: same as the pictures - fetched at boot (quietly if a set is cached), any clip
  whose size changed is downloaded into `/rumours` on LittleFS, and a copy of the list is kept
  so it works offline. First open with nothing cached shows "fetching n of m".
- **Decoding**: the Helix fixed-point MP3 decoder vendored in `Badge\src\helix` (RealNetworks
  RPSL/RCSL licence, taken from ESP8266Audio), driven by `Badge\src\mp3stream.c`. Tested on the
  PC on the real clips: **correlation 1.0000 with ffmpeg's decode at zero lag**, durations
  matching, clean under AddressSanitizer, including the 24 kHz stereo drum roll.
  ESP8266Audio itself is not used: its I2S output links the legacy ESP-IDF I2S driver, which
  aborts at boot next to the new driver this sketch uses.
- **Playback**: a FreeRTOS task on core 0 decodes from LittleFS and writes to the same 16 kHz
  I2S/ES8311 path as the Decider (`decAudioInit()`), so no clock change between apps. It never
  plays over the drum roll, and the amp is switched off after each clip.
- Random order is a shuffled deck: every rumour plays once before any repeats, and a new deck
  never starts with the one that just played.
- Build: **2,544,003 bytes** (53%), globals 22%.

## v2.9 - shorter Settings menu

Settings now has two rows: **Wi-Fi setup on phone** and **Close** (taller, centred in the circle).
**Pictures** was removed from it (it is on the home carousel) and the **Touch test** screen was
removed from the firmware altogether; tap timings are still logged on serial. Build 2,542,855 bytes.

## Next steps

- Flash v2 and read the boot log's `full frame` and `composed + flush` timings; confirm the panel
  is clean at 80 MHz.
- The CYD and the 3.5" ESP32-S3 board carry the rest of the kit — see `..\CYBERPUNK.md`.
