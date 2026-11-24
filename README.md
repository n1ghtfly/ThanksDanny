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

## v3.0 - startup splash

At power-on the badge now shows a **startup picture** (the Sons of Sudo bikers outside the
Biker Clubhouse) and plays a **startup sound** (a 7 s superhero intro), then goes to the home
screen. A tap or BOOT skips it. `preview_startup.png` is the picture as it appears on the glass.

- Source: `Downloads\ThanksDanny\startup\` (the first picture and the first .mp3 in it).
  `tools\prep_startup.py` crops the 1792x2390 portrait to a square near the top (`BIAS` 0.08,
  picked from renders through the round mask: sign, three faces and the "EST. 2010" patch all on
  the glass), blacks out the corners and stores it as raw RGB565 (424 kB); the sound is
  re-encoded to 16 kHz mono 48 kbit/s MP3 (41 kB). Both go into `Badge\startup_assets.h`.
- Compiled in, so the splash appears ~50 ms after the panel starts - before Wi-Fi, and even after
  a filesystem reflash. The picture is a memcpy + one flush, no decode.
- Sequence in `setup()`: panel up -> **splash picture** -> filesystem, touch -> **sound starts**
  (needs the I2C bus that touch brings up) -> Wi-Fi connect begins -> **wait** until the sound
  ends (at least 1.8 s, at most 15 s; a tap or BOOT skips, and the skipping finger is not passed
  on as a tap) -> home screen. Wi-Fi keeps connecting in the background throughout.
- The sound plays through the Rumours player plumbing (`rumPcm`, `rumBusy`/`rumStop`) on core 0;
  decoded on the PC with the badge's decoder: 195 frames, 7.02 s, no errors.
- Build: **3,020,147 bytes** (64% of the app partition).

## v3.1 - SLAP: the badge network (MQTT)

The badges (Claudio, Danny, Walter) now talk to each other through your own Mosquitto. First feature:
**the remote slap**. `preview_slap.png` shows the pick screen, the armed screen and the incoming slap.

- **SLAP** is a fifth carousel app. It shows the other two badges with an online dot; tap one, then
  **swing your badge like a slap**. The QMI8658 gyro measures the swing (a turn over 480 dps starts it,
  the peak over the next 120 ms sets the strength, 2000 dps = 10/10). A SLAP button sends a polite 5
  without swinging. 2.5 s cooldown between slaps.
- **Getting slapped** interrupts any screen (the slideshow checks between pictures): a hand swings in,
  the screen shakes in proportion to the strength, a slap sound plays louder for harder slaps, the
  counter goes up, and **SLAP BACK** arms a return slap. Slaps sent while a badge is off are kept by the
  broker and land when it comes back ("x3, hardest 8/10").
- **Settings** gained *This badge: <name>* (tap to cycle) and *Slap sound: ON/OFF* (meeting mode).
  The broker host, port, TLS and login are on the phone setup page (Settings -> Setup on phone), stored
  on the badge only; the password is write-only. After an MQTT-only save the setup network closes by itself.
- **Home pill** shows "sos 2/3 online" once connected.
- **Protocol**: `sos/<to>/inbox/<from>` = `slap|<1-10>` (QoS 1, persistent sessions), `sos/presence/<name>`
  = `1`/`0` retained with `0` as the last will. The ACL in `server/mosquitto/acl` lets `<from>` only be the
  sender's own login. Tested on Mosquitto 2.0.22 built from source: delivery, spoofing blocked, inbox
  snooping blocked, presence spoofing blocked, offline delivery, last will.
- **MQTT client**: ESP-IDF's esp-mqtt (part of the ESP32 core, no library), in `Badge/src/sosnet.cpp`, TLS
  checked against the core's public CA bundle - so the broker needs a public certificate (Let's Encrypt via
  the DuckDNS DNS challenge, see `server/mosquitto/README.md`).
- **Sounds** (`Badge/sos_sounds.h`) are synthesized by `tools/make_sos_sounds.py`: whoosh + smack + thump.
- **Broker address** default comes from `Badge/sos_local.h`, which is git-ignored, so the address is not
  in the public repository; without that file the host is typed on the setup page.
- **PC tool**: `tools/sos_cli.py` (needs `pip install paho-mqtt`): `watch`, `who`, `slap <name> [1-10]` -
  tested against the broker above.
- Build: **3,110,631 bytes** (65%), globals 23%.

## v3.2 - screensaver

`Badge/Screensaver.ino`; `preview_screensaver.png` shows it.

- After **60 s without a touch** on a quiet screen (home, settings, a picture, a decider result, a slap you
  got, Rumours when silent, SLAP while picking) the panel dims to 40/255 and shows a black screen with a
  seven-segment clock, the date and a dot per member (lit = online). The block moves to a random spot
  within +-30 px every minute, so nothing burns into the AMOLED. After **5 more minutes** the panel sleeps
  (DISPOFF + SLPIN).
- **Wakes** on a tap (swallowed: it does not also press what is under it), BOOT, picking the badge up
  (gyro over 120 dps) or an incoming slap (which then takes over as usual). The screen that was up is
  copied to PSRAM when the saver starts and copied back on wake, so every app comes back exactly as it was.
- Refreshes that arrive while it is up (presence changes, Wi-Fi coming up) are noted and the screen is
  redrawn on wake instead of restored.
- The clock is local time: SNTP is now asked with the Europe/Amsterdam zone (`configTzTime`), summer time
  included. Before Wi-Fi has set the clock it shows "zzz".
- Tunables at the top of `Screensaver.ino`: `SAVER_AFTER_MS`, `SAVER_OFF_MS`, `SAVER_BRIGHT`, `SAVER_WAKE_DPS`.
- Build: **3,112,971 bytes** (65%), globals 23%.

## v3.3 - SOS MARKETS (Bloomberg-style screensaver)

The screensaver's clock became a market screen (`preview_screensaver.png`); everything else about the
saver (when it starts, waking, panel sleep, restoring the screen) is unchanged from v3.2.

- **Header** "SOS MARKETS 20:41" (amber), **main quote** in the centre: symbol, price, change (green
  up-triangle / red down-triangle, or a status) and an analyst quip, plus market breadth (up / down count).
  The main quote moves on to the next index every 12 s, and all prices tick at the same moment.
- **Crawl**: an amber band along the bottom edge scrolls every index and a headline every four quotes
  ("FED HOLDS COFFEE RATE AT ZERO"). ~60 px/s at ~30 fps; only the 466x30 band is redrawn and sent
  (`flushRect`), so it costs a few ms per frame. Rebuilt with fresh prices each time it has run through.
- **Indices** (`SV[]` in `Screensaver.ino`): random walks with a personality - $DANNY only goes up,
  $MEETING only down (floor -99.9%), $BUGS to the moon; fixed statuses $COFFEE LIMIT DOWN, $DEPLOY HALTED,
  $RUMOUR UNCONFIRMED. Live ones: $CLAUDIO / $DANNY / $WALTER show **MKT CLOSED** while that badge is
  offline, $SLAP is this badge's real received-slap count, $BEER is PRE-MARKET until 17:00.
- Burn-in: header and quote move to a random spot within +-30 px every minute; the band keeps moving.
- Build: **3,114,995 bytes** (66%), globals 24%.

## v3.4 - screensaver ignores movement

Moving the badge (walking with it on a lanyard) kept waking the screensaver. It now wakes only on a
screen touch or the BOOT button - and an incoming slap still takes over the screen as before.
Build: **3,114,911 bytes** (66%).

## v3.5 - removing pictures and rumours actually removes them; one-click update

- Before, the badge only ever added: a picture or rumour taken off the site was no longer listed, but its
  file stayed in flash (wasting space) and a removed picture could still appear when offline. Now, after a
  successful list fetch, Pictures and Rumours delete every cached file that is not on the list
  (`pruneDir()` in `Gallery.ino`). Never when offline or when the list fetch failed.
- `tools/prep_github_pics.py` deletes old `pNN.jpg` files from `github-pics\` that no longer correspond to
  a source picture, so the site holds exactly the current set.
- **`update_badge_content.cmd`**: prep pictures, prep rumours (skipped with a note if ffmpeg is missing),
  publish. `push_pictures_site.cmd`'s commit message is now generic.
- Build: **3,116,427 bytes** (66%).

## v3.6 - carousel freezes

- **Start-up update off the UI**: the check for new pictures and rumours ran inside `loop()` right after
  Wi-Fi connected - 2-4 HTTPS handshakes plus up to 6 s waiting for SNTP, with no touch polling, so the
  carousel froze for several seconds after every boot. It now runs in a task on core 0 (priority 1, below
  the audio tasks) and never draws (`contentSyncStart()` in `Gallery.ino`). Pictures and Rumours wait for it
  if opened meanwhile ("updating... picture 3 of 8"). First boot (nothing cached) still syncs on screen.
  Also fixed: the quiet sync called `showScreen(ST_GALLERY_SYNC)` per picture, silently switching the
  screen state away from home while home was on the glass.
- **MQTT task priority 1** (was 5): the core's esp-mqtt task is not pinned to a core, so a reconnect's TLS
  handshake could pre-empt `loop()` on the UI core for a second or more.
- **Stall log**: `stall  : loop blocked N ms (screen S)` whenever one pass of `loop()` takes over 150 ms.
  Expected for app openings and animations; anything on the home screen with no tap before it is a lead.
- Build: **3,117,299 bytes** (66%).

## v3.7 - Danny's official reason (No-as-a-Service)

`preview_nope.png`. When the Decider lands on DISAPPROVED, after the usual 1.2 s an **OFFICIAL REASON**
card slides up over the bottom of the badge (the thumb and QUALITY CONTROL, not Danny's face), above
AGAIN | HOME.

- Reasons: [No-as-a-Service](https://github.com/hotheadhacker/no-as-a-service) (MIT, (c) 2026 hotheadhacker).
  `tools/prep_no_reasons.py` downloads its `reasons.json`, straightens curly quotes/dashes for the ASCII
  font and keeps those that fit the card (4 lines x 24 characters, wrapped at spaces exactly like the
  badge): 955 of 1055. Output `github-pics/no/reasons.txt` (56 kB) + `no/index.txt` (its byte size).
- The badge never calls their API: the background update fetches `no/index.txt` and downloads
  `reasons.txt` into `/no/` only when the size changed (`nopeSync()` in `Badge/Nope.ino`). Picking one is a
  random seek + next line - instant and offline. Five built-in reasons cover the time before the first download.
- `update_badge_content.cmd` has a step for it (3/4).
- Build: **3,119,667 bytes** (66%).

## v3.8 - Ask Danny

- The Decider is called **ASK DANNY** on the home screen (and in the SOS MARKETS headline). Code names
  (`Decider.ino`, `ST_DECIDER`) are unchanged.
- **Approved** now also gets an OFFICIAL REASON card, in green: one of 40 yeses ("I'm in, and I brought
  snacks."), compiled in as `YES_REASONS` in `Nope.ino` (em dashes straightened for the ASCII font; all fit
  the 4 x 24 card). Disapproved keeps the red card with the No-as-a-Service excuses. `preview_yes.png`.
- Build: **3,122,071 bytes** (66%).

## v3.9 - Ask the Council (poll website)

`preview_vote.png` (badge), `preview_poll_web.png` (website).

- **Website** `web/poll.html` (one file, no libraries - it carries a ~150-line MQTT 3.1.1 client over
  WebSocket). Log in with your own badge login, ask a question (up to 96 characters - checked live against
  the badge's 4 x 24 word-wrap) with 2-4 answers (presets or your own, 12 characters each), watch the votes
  come in, vote yourself, close your question. Verdict when everyone voted ("Tie ... Ask Danny."). The broker
  address is typed in (remembered by the browser); the password is never stored. Published on the Pages site
  as `poll/index.html`.
- **Badges** (`Badge/Vote.ino`): an open question they have not answered pops up wherever they are (not over
  a slap, an armed swing, a playing rumour or the setup pages) with a swish: "<NAME> ASKS:", the question, one
  button per answer, LATER. After voting: live bars with initials, "waiting for ...". LATER shelves it until
  the next start-up. Retained topics, so a badge that was off still gets open questions, and knows from its own
  retained vote that it already answered.
- **Protocol**: `sos/poll/<asker>` = `<id>|<question>|<a1>|<a2>[|<a3>|<a4>]` retained (empty = closed);
  `sos/vote/<asker>/<voter>` = `<id>|<index>` retained. ACL: ask and vote only under your own name.
- `sosnet`: message payloads 64 -> 208 bytes, up to 8 subscriptions, receive queue 24.
- **Tested** end to end on Mosquitto 2.0.22 with the new ACL: the page in headless Chromium (through a
  WebSocket bridge) as Claudio, two simulated badges as Danny and Walter: question delivered in the badge
  format, votes counted live on the page, verdict, close; wrong password reported; Danny voting as Walter and
  Danny posting a question as Claudio both dropped by the broker.
- Server: `listener 9001` + `protocol websockets`, ACL lines, port 9001 in docker-compose, NPM proxy host with
  Websockets Support (`server/mosquitto/README.md` section 9).
- Build: **3,125,739 bytes** (66%).

## v3.10 - no start-up splash

The power-on picture and sound (v3.0) are gone: the badge goes straight to the home screen. Removed
`Badge/Startup.ino`, `Badge/startup_assets.h`, `tools/prep_startup.py` and `preview_startup.png`.
Build: **2,648,415 bytes** (56%, was 66% - the splash was ~470 kB of compiled-in picture and sound).

## v3.11 - pictures, rumours and excuses straight from the repository

New pictures were pushed but never reached the badges: GitHub Pages only publishes a push after a GitHub
Actions job rebuilds the site, and during GitHub's Actions incident of 5 Oct 2026 that job sat queued and
was then cancelled - so `list.txt` on the Pages site stayed at the old 6 pictures. The badges now download
from `https://raw.githubusercontent.com/n1ghtfly/danny-pics-7f3c9a/main` (`PIC_HOST` in `Gallery.ino`),
which serves pushed files directly (cached up to ~5 minutes) with no build step. The poll website still
lives on Pages (it is a web page, and it rarely changes).

## v3.12 - RADAR app (live aircraft)

`preview_radar.png`. A sixth carousel app, modelled on
[MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar) (MIT, (c) 2026 MatixYo) - same
idea, data source and look, rebuilt for this badge (466 px instead of 240, Arduino_GFX instead of LovyanGFX,
no ArduinoJson).

- **Data**: `https://opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<nm>` every 5 s while the app is open
  (radius = outer ring x 1.3), in a task on core 0 (`Badge/Radar.ino`). HTTP/1.0 so the body is never chunked;
  certificate checked against the core's CA bundle, unvalidated retry only if that fails to connect.
  Aircraft on the ground are left out; up to 96 kept.
- **Parser**: `Badge/src/adsbparse.c` - a ~170-line streaming JSON reader in plain C that keeps lat/lon,
  track (else true_heading), gs (else tas), alt_baro (else alt_geom), seen_pos, flight (else hex), t, and skips
  everything else including nested arrays/objects. Host test `tools/test_adsbparse.c`
  (`gcc -I../Badge/src test_adsbparse.c ../Badge/src/adsbparse.c -lm`): readsb-format sample with nav_modes,
  mlat/tisb arrays, a nested object, escaped quotes, "ground", null, missing fields, limit, empty and error
  documents, a truncated body and an HTML error page - all pass.
- **Screen**: four green rings (outer radius 200 px), N/E/W outside, S inside (HOME sits below), range label on
  the east spoke. Inside the ring: red triangle along the track, magenta vector = 1 min of travel x 1.5 (clipped
  at the ring), callsign (size 2) + "altitude ft type" (size 1, amber). Outside the ring: red rim dot at the
  bearing. Positions are dead-reckoned from the fix (age + seen_pos) along track at ground speed, redrawn every
  300 ms. The whole picture shifts a few px every minute (burn-in: this screen is meant to stay on).
- **Controls**: tap = next range (ring 3 = 5 / 10 / 15 / 25 km, saved); HOME pill or BOOT = home.
- **Location**: phone setup page -> "Plane radar" -> one field ("52.3676, 4.9041", decimal commas and the
  "52,3676,4,9041" form accepted). Stored in NVS "radar", never logged, never in the repo. Optional default in
  `Badge/sos_local.h`: `#define RADAR_DEFAULT_LAT ...` / `RADAR_DEFAULT_LON ...`.
- Not ported: the runway overlay (a 2,900-line airport table) and the miles option.
- Build: **2,675,155 bytes** (56%).

## v3.13 - setup page always reachable

- The setup page (Wi-Fi, badge network, radar location) is now served on every network the badge is on:
  its own setup network as before, and - once connected - its address on your Wi-Fi, plus mDNS
  `http://badge-<name>.local` (`thanksdanny.local` before a name is chosen). The DHCP hostname is the same,
  so routers list it as `badge-danny` etc. (`webBegin()` / `webHostname()` in `Badge.ino`.)
- Settings -> Setup: on Wi-Fi it just shows those two addresses (no scan, no setup network); without Wi-Fi it
  raises `ThanksDanny-setup` as before.
- Safety now that anyone on the same Wi-Fi can open the page: changing the broker host, port or TLS requires
  typing the password again (otherwise the saved password could be sent to someone else's broker). Passwords
  are still write-only on the page and never logged. Anyone on the same Wi-Fi can still change the Wi-Fi or the
  radar location - fine at home; a PIN would be the next step if the badges live on shared networks.
- The mDNS name follows the badge name chosen in Settings from the next start-up.
- Build: **2,713,035 bytes** (57%).

## v3.14 - bigger, sharper text on the home screen and in Settings

`preview_fonts.png` (before / after). The built-in 6x8 font only scales in whole steps, so "10% bigger" uses
a real typeface: DejaVu Sans Bold (Bitstream Vera licence, free to embed) converted to Adafruit-GFX fonts by
`tools/make_ui_fonts.py` into `Badge/ui_fonts.h`, each sized by capital height to 110% of the text it replaces:
UiSmall caps 8 px (was 7), UiBig 15 px (was 14), UiTitle 23 px (was 21). About 8 kB of flash.

- Used for the home pill (app name + network line) and the whole Settings menu. `uiText()` / `uiCentred()` in
  `Badge.ino` take the top of the capitals (like `setCursor` with the built-in font) and always restore the
  built-in font, so every other screen is unchanged.
- The "not connected" line now reads "<network> - not connected" so a long network name still fits.
- Build: **2,724,427 bytes** (57%).

## v3.15 - Radar: tap a plane for details

`preview_radar_detail.png`. Tap a plane (or its callsign label) on the radar: a detail screen with callsign,
airline, route (origin -> destination with cities), aircraft (type description + registration), and live
altitude with climb/descent (fpm), speed (kt and km/h), heading, distance and direction from you, squawk
(7500 / 7600 / 7700 shown red as EMERGENCY), owner. Refreshes every second from the live feed; if the plane
leaves the fetched area it says "out of range - last seen Ns ago". Any tap = back to the radar.

- **Live fields** come from the adsb.fi feed: `adsbparse.c` now also keeps hex, r, desc, ownOp, squawk and
  baro_rate (else geom_rate).
- **Route, airline, aircraft** come from [adsbdb.com](https://www.adsbdb.com) (free, no key), looked up once
  when a plane is opened, by the radar's fetch task on core 0: `/v0/callsign/<callsign>` and
  `/v0/aircraft/<hex>`. Read with `json_flat_field()` (new in `adsbparse.c`). No callsign -> only the aircraft.
- Host test extended (`tools/test_adsbparse.c`): the new feed fields, and `json_flat_field` on adsbdb-style
  answers (nested airline/origin/destination, exact key match, null, unknown callsign, truncation). All pass.
- The aircraft lists (3 x 96 entries, ~130 bytes each now) moved to PSRAM.
- Build: **2,730,751 bytes** (57%).

## v3.16 - snappier carousel swipe

The slide redrew and sent a 211-row band (rows 214-424) five times per swipe, each time copied into a temp
buffer first, and only started after 45 px of finger travel. Now:

- **Starts sooner**: a swipe is recognised after 30 px (`SWIPE_PX`, was 45).
- **Less per frame**: frames redraw and send only the icon strip (rows 224-355, `ICONS_Y0/1`); the dots and the
  name pill (`drawCarouselInfo()`) are redrawn only on the frame where the app in the middle changes.
- **No copy**: `flushRect()` sends full-width bands straight from the framebuffer (they are contiguous), instead
  of copying them into `flushTmp` first. Helps every full-width partial update, not only the carousel.
- **4 frames** instead of 5 (ease-out: 44% / 75% / 94% / 100%).
- Expected: roughly 2-3x faster per swipe; the serial log's `ui     : carousel -> NAME (N ms)` line shows the
  real number.
- Build: **2,730,991 bytes** (57%).

## v3.17 - bigger app icons and UI text

`preview_home_bigger.png`.
- **Icons**: carousel slot radii 20 / 36 / 60 -> 24 / 42 / 70 (middle +17%, sides +17%); slots moved up
  (`SLOT_Y` 310 / 296 / 284) so the dots (y 362) and the name pill (y 374) still fit below, and the middle icon
  stays clear of the wallpaper's "SUDO". Slide strip `ICONS_Y0` 200 (covers the press ring), `CAROUSEL_Y0` 200.
- **UI fonts** one step up (`tools/make_ui_fonts.py`): caps UiSmall 8 -> 9 px, UiBig 15 -> 17 px,
  UiTitle 23 -> 26 px - home pill, Settings and the radar detail screen. All Settings lines still fit their
  rows (longest subtitle 299 of 322 px).
- Build: **2,733,279 bytes** (57%).

## v3.18 - ORACLE app ("Network Socrates" 8-ball)

`preview_oracle.png` (swirling / answer / answer). A seventh carousel app, between Radar and Settings.

- **Ask**: tap the sphere, or shake the badge - the QMI8658 accelerometer (`sosAccelG()` in `Sos.ino`), over
  1.9 g or under 0.25 g, with a 1.2 s cooldown. A whoosh plays (`sosSwish()`, respects Slap sound OFF).
- **Animation** (~2 s, BOOT still works): the window closes, the marbled sphere spins fast and swirls (each
  texture row twisted by a different amount), slows down, the window opens from a dot and the answer floats up
  while it brightens. Never the same answer twice in a row.
- **Sphere**: no per-frame maths. On first open (~0.5 s) a 256x256 marble texture is generated in 10 shades
  (8 Lambert levels + 2 highlight levels, 1.3 MB PSRAM) and a table maps each of the disc's ~102k pixels to
  (u, v, shade) of a sphere lit from the upper left. A frame = one table walk + a 362-row full-width flush.
- **Answers**: 59 in `ORA_SAYS[]` - the three from the brief plus Socratic rebukes, classic-8-ball parodies and
  excuses. Wrapped to the round window in UiBig, or UiSmall when an answer needs more than 4 lines or has a
  word too wide for the window (checked for all 59).
- Screensaver may start on the Oracle screen. Icon: an 8-ball.
- Build: **2,743,139 bytes** (58%).

## v3.19 - Oracle: bigger text, new quotes

`preview_oracle_v319.png` (short decree / typical quote / longest quote).

- **Readable text**: the answer window grows from radius 118 to 162 (a thin marble rim stays), and the 9 px
  UiSmall fallback is gone. Font choice per answer, biggest that fits: UiTitle (caps 26) for the short decrees,
  the new **UiOracle** (DejaVu Sans Bold 27 px, caps 20, generated by `tools/make_ui_fonts.py`, +4 KB) for most
  quotes, UiBig (caps 17) only as the last resort. Simulated for all quotes: 11 title, 30 UiOracle, 2 UiBig.
- **Round wrapping**: `oraWrap()` now gives every line the chord width of a radius-150 circle at that line's
  height, trying 1, 2, ... lines until the text fits - so middle lines are wide and top/bottom lines narrow.
- **Quotes**: `ORA_SAYS[]` replaced with the owner's 43 (Socratic scorn, layer 1, routing, OS daemons, short
  decrees). Code-style backticks became plain single quotes (the screen font is ASCII only).
- Idle "ASK" screen uses UiBig for "tap or shake / the oracle".
- Build: **2,747,759 bytes** (58%).

## v3.20 - double tap opens an app

- Home carousel: a single tap on the big middle icon now only **arms** it (the white highlight ring stays on);
  a second tap within **450 ms** (`DBL_TAP_MS`) opens the app. The second tap is caught on the *press*
  in `pollTouch()`, so the app opens the moment the finger lands - no waiting for the lift.
- No second tap in time: `loop()` takes the ring off again (`homePressClear()`). A swipe or a tap anywhere
  else disarms it. Side icons still move with one tap; every other screen is unchanged.
- Serial log: `-> (home: armed - tap again to open)` and `tap    : double tap -> open <APP>`.
- Build: **2,748,007 bytes** (58%).

## v3.21 - radar sweep

`preview_radar_sweep.gif`. The radar now looks like a real scope:

- A green **beam turns clockwise** once every 4 s (`RADAR_SWEEP_MS`), with a 70 degree trail fading into the
  background (`radarSweep()`: 28 thin wedges, drawn under the rings and the planes).
- **Echoes**: each aircraft (and each rim dot) is brightest just after the beam passes over its bearing and dims
  to 35% over one turn (`radarSince()`, `radarEcho()`). Callsigns and altitudes stay full brightness.
- Frame cadence 300 ms -> **40 ms** (~25 fps). The beam angle comes from `millis()`, so it turns at the right
  speed even if a frame is slow; `radar  : sweep frame N ms` in the serial log once a minute shows the cost.
  The detail screen still redraws once a second.
- Build: **2,749,067 bytes** (58%).

## v3.22 - carousel freeze after the first swipe (serial logging blocked)

Symptom: the first swipe works, then the carousel ignores swipes for several seconds.

Cause: `Serial` is the ESP32-S3's built-in USB-CDC (HWCDC). When the badge is plugged into a PC but **no
serial monitor has the port open**, nothing reads the data; once the 256-byte TX ring is full, every
`Serial.print` waits in `HWCDC::write()` for up to 20 x `tx_timeout_ms` (default 100 ms) = **2 s** before
giving up ("host backpressure" path in core 3.3's HWCDC.cpp). A swipe logs two lines (`swipe :`,
`ui : carousel ->`), so `loop()` - and with it touch polling - stood still for ~4 s. With the monitor open
(as after `flash_badge.cmd`) it never happened, which is why it looked random.

Fix: `Serial.setTxTimeoutMs(0)` right after `Serial.begin()`. A log line that does not fit is dropped
instead of waited for. Logging with the monitor open is unchanged.
- Build: **2,749,087 bytes** (58%).

## v3.23 - clap detection: "Thanks Danny!"

`preview_thanks_clap.png`. Clap once near the badge and it says **"Thanks Danny!"** (and, on the home
screen or the screensaver, shows a THANKS DANNY! splash with confetti for 1.8 s).

- **Microphones**: the board's two MEMS mics go through an **ES7210** ADC (I2C 0x40). Its data line is
  GPIO 10 - already the DIN of the Decider's I2S port (`decI2s`), so no new pins: `thanksStart()` brings
  the audio path up at boot, configures the ES7210 (slave, 16 kHz, 16-bit, mic gain 24 dB) and starts
  reading the RX side. Driver `src/es7210.cpp/.h` + `src/audio_hal.h`: Espressif's (MIT), from Waveshare's
  `06_ES7210` Arduino example for this board, unchanged.
- **Detector** (`Thanks.ino`, task on core 0, 8 ms blocks): a clap must (1) peak above `CLAP_MIN_PEAK`,
  (2) be `CLAP_RATIO` x the room-noise average, (3) be `CLAP_ATTACK` x the previous block (sharp start),
  (4) be BRIGHT - rms of the sample differences / rms >= `CLAP_HF_MIN` (claps ~1.4, voiced speech <0.5),
  (5) die down to 25% within `CLAP_DECAY_MS`. Then 2 s cooldown. Deaf while the badge's own sounds play
  (`sndBusy` / `sosSndBusy` / `rumBusy`) and 0.5 s after.
  Simulated on host first (synthetic claps with and without room echo, Piper TTS speech at 1x/3x/6x,
  "Ha!"/"stop!"/"cut!" plosives, a door slam): without test (4) speech gave 5 false claps; with it 0,
  and all 4 claps found. Not caught: a clap *during* loud talking (masked).
- **Voice**: `thanks_sound.h`, 0.86 s, 16 kHz mono PCM in flash (27 kB), played with `sosPlay()` - so the
  slap **mute** setting also silences it (the splash still shows). Made offline with Piper TTS, voice
  en-us-ryan-high (CC BY-NC-SA 4.0). `tools/make_thanks_sound.py <any audio file>` regenerates it - e.g.
  from a phone recording of Danny himself.
- Serial: `clap   :` lines - room noise once a minute, every clap with its numbers, and every rejected
  candidate with the reason (too dull / loud too long). Tune the `CLAP_*` defines from those.
- Build: **2,781,511 bytes** (58%).

## v3.24 - clap: answer after the applause, not during it

Hardware feedback: detection works, but the badge started talking on the first clap while people were
still clapping, so "thanks" was lost in the noise.

- Claps are now **counted**, not answered: after each clap the detector is deaf for only `CLAP_GAP_MS`
  (120 ms) so it hears the next one. It answers once nothing louder than 4x the room noise has been heard
  for `CLAP_QUIET_MS` (**700 ms**), or at the latest `CLAP_ROUND_MAX` (6 s) after the first clap. Then the
  2 s cooldown as before.
- The splash says how many it heard: `*clap*`, `*clap* *clap*`, `*clap* x5`.
- Simulated: 1 clap -> answer 0.86 s later; 5 claps 0.3 s apart -> one answer 0.78 s after the last;
  12 fast claps -> one answer 0.8 s after the last.
- Serial: `clap   : quiet again after N clap(s) in T ms - answering`.
- Build: **2,781,939 bytes** (58%).

## v3.25 - clap: "often does not react"

Hardware feedback after v3.24: claps were often ignored, as if the detector were stuck. Three causes, all
reproduced in the host simulation first:

- **Waiting for a quiet room**: v3.24 answered once nothing louder than 4x the room noise had been heard
  for 700 ms. Talking or office noise kept resetting that, so the answer often only came at the 6 s limit.
  Now: answer **800 ms after the last clap** heard (`lastClap`), whatever else the room does.
- **Clap split over two blocks**: a clap starting at the end of an 8 ms block made that block the
  "onset" with a low level, and the decay test (25% of the onset) then failed. Simulated with claps at a
  block edge: v3.24 found 1 of 5, v3.25 5 of 5. The event now tracks its **loudest** block.
- **Clipping**: a clap close to the mics clips at 24 dB gain; flat-topped samples fail the brightness test.
  A clipped onset (>= 4 samples at full scale) now skips that test, and gets 160 ms instead of 100 ms to
  die away. Logged as brightness -1.
- Build: **2,782,031 bytes** (58%).

## v3.26 - clap detection removed

The owner tried v3.23-v3.25 on the badge and decided against it: removed again. Gone: `Thanks.ino`,
`thanks_sound.h`, `src/es7210.*`, `src/audio_hal.h`, `tools/make_thanks_sound.py`, the preview, and the
`thanksStart()` / `thanksTick()` calls. The microphones are unused again (the ES7210 is left unconfigured,
as before v3.23). The v3.23-v3.25 sections above stay as a record - the driver came from Waveshare's
`06_ES7210` example if it is ever wanted again.
- Build: **2,749,067 bytes** (58%).

## v3.27 - CLOCK app: steampunk pocket watch with a moving movement

`preview_clock.png`, `preview_clock.gif`. New app **CLOCK** (carousel, before SETTINGS): the owner's gilded
Roman-numeral dial (`assets/clock_dial_src.jpg`), with the movement in the middle window really running.

- **Dial**: cut out of the owner's picture (circle 209.5, 229, r 197.5 in the source), scaled to 466 px.
  The bezel and numeral ring are kept; the window (r < 137) gets a new movement plate - dark gilt with
  perlage, recessed wheel sinks, a hairspring - because the photographed movement cannot turn. Stored as
  an 87 kB JPEG in flash, decoded once into PSRAM (`clkBg`, 434 kB) with the gallery's `decDest` path.
- **Movement** (all rendered by `tools/make_clock_assets.py` at 4x, scaled down = anti-aliased, RGB565 +
  2-bit alpha): 8 front wheels and 4 lower-level wheels sharing their arbors, a balance wheel with timing
  screws, two steel bridges (polished chamfer, Geneva stripes, ruby jewels, blued screws), gilt Breguet
  hour and minute hands and a crimson seconds hand with counterweight. The train is laid out and phased
  in the script so the teeth mesh: each child is placed at exactly r1 + r2, its start angle solved so a
  tooth meets a gap, its speed -(n1/n2) of its parent. No two non-meshing wheels overlap (checked).
- **Ticking**: 4 beats a second. Each beat the train and the seconds hand jump (ease-out over 70 ms);
  the balance swings once per 2 beats. Wheel A turns once a minute.
- **Rendering** (`Clock.ino`): per frame copy the dial into the canvas, rotate every sprite by inverse
  mapping (16.16 fixed point, nearest pixel, only inside each sprite's `reach`; hands use the box of the
  rotated rectangle), the static bridges by per-row spans, the hands, a ruby centre cap, flush. Aim
  20 fps, but always >= 15 ms of `loop()` between frames so touch stays responsive. Serial:
  `clock  : HH:MM:SS, frame N ms avg` every ~20 s.
- **Time**: the existing NTP sync (`configTzTime`, Europe/Amsterdam). Not synced yet: hands at 10:08 and
  "WAITING FOR TIME".
- **Burn-in**: the screensaver does not take over this screen (it is meant to be left on), so the whole
  picture shifts by up to 2 px every 2 minutes (9 positions).
- Tap anywhere: home.
- `clock_assets.h` is included from `Badge.ino`: Arduino writes the auto-prototypes for `Clock.ino` above
  that file's own `#include`s, and `clkBlit()` takes a `ClkSprite *`.
- Build: **3,156,015 bytes** (66%; the clock adds ~400 kB of images).

## v3.28 - clock: sweeping seconds, faster frames

Hardware feedback on v3.27: the seconds were not smooth.

- **Sweep instead of tick**: v3.27 ticked 4 beats/s with an ease-out jump; with a frame rate that is not a
  multiple of 4 (and varies), each beat was shown for a different number of frames - visible stutter.
  Now the seconds hand sweeps continuously (time to the microsecond from `gettimeofday`), the wheels turn
  smoothly, the minute and hour hands move continuously too. The balance still swings at 2 Hz.
- **Dirty rectangle**: only the movement window plus the boxes of the hands now and last frame are
  restored from the dial and sent to the panel (as one full-width band, which `flushRect` sends without
  copying). Full redraw only on the first frame and when the burn-in shift moves (every 2 minutes).
- **Sprites in PSRAM**: copied from flash once when the app opens (`clkLoadSprites`, ~310 kB). The
  rotations read the sprites in scattered order, and octal PSRAM fills cache misses ~4x faster than
  quad flash.
- Serial: `clock  : HH:MM:SS, frame N ms avg = restore a + movement b + hands c + flush d` every ~20 s,
  so the next optimisation can aim at the real cost.
- Build: **3,156,607 bytes** (66%).

## v3.29 - clock face: cyberpunk radar HUD (replaces the steampunk watch)

`preview_clock.png`, `preview_clock.gif`. The owner sent a reference picture of a green/amber HUD clock; it
is rebuilt here as a live face (the picture itself has a fixed time, garbled text and a made-up IP in it).

- **Static** (one 103 kB JPEG, `tools/make_clock_assets.py`): orange and green rings, a tick ring with
  decorative arcs, a faint polar grid, minute numbers 60/5/.../55, scanlines.
- **Every frame** (`Clock.ino`):
  - radar sweep: green light *added* behind a beam turning every 6 s (90 degree trail), from a per-pixel
    angle table built when the app opens - the grid and numbers shine through it;
  - outer text ring scrolling round (22 px/s): SONS OF SUDO * SYS.LEVEL: ROOT * BGP PEERING: STABLE * the
    Socrates quote in orange * IT IS ALWAYS DNS. A pre-rendered strip mapped onto the ring by a second table;
  - seconds ring: 120 amber bars filling up through the minute, the newest four brightest;
  - the time in Black Ops One stencil digits with a pre-rendered glow, added like light, fixed slots so
    they do not jiggle;
  - live HUD: SYSTEM STATUS SECURE / NO LINK (this badge's MQTT link, `sosLinkUp()`), SONS OF SUDO //
    <badge name>, Wi-Fi RSSI, uptime, STARDATE yyyy.mm.dd + weekday, CET/CEST.
- Fonts in `assets/fonts/` with their SIL OFL licences: Black Ops One (digits), Share Tech Mono (HUD, as
  GFX fonts `HudSmall` / `HudMid`).
- Kept from v3.28: adaptive frame pacing, burn-in shift, frame timing in the serial log (now background /
  sweep+ring+bars / text+time / flush).
- The steampunk face is gone (its source picture removed from the repo); it is in git history (v3.27-3.28).
- Build: **2,957,715 bytes** (62%).

## v3.30 - clock: smoother, no more hangs

Hardware feedback on v3.29: the clock sometimes hangs, the animation could be smoother.

Cause: a full-screen flush costs ~46 ms on this panel (byte-swapped through 1024-pixel DMA chunks), and
v3.29 sent the whole 466 x 466 screen every frame - ~12 fps, with loop() busy almost all the time, so any
other work at the same moment (Wi-Fi, MQTT, a sync) showed as a hang.

- Every frame now restores, draws and sends only the **centre**: the sweep disk with the time and HUD text
  (a 314 x 314 box via `flushRect`, ~45% of the screen).
- **Seconds bars** are redrawn only when they change (twice a second: the new bar and the end of the bright
  head) and sent as a small patch; at the top of the minute all 120 at once (one full flush).
- The **text ring** no longer scrolls: it is drawn on full frames only (first frame, burn-in shift every
  2 minutes). Scrolling it meant sending the whole ring every frame.
- **Wi-Fi RSSI, link state, badge name** are read once a second instead of every frame - those calls go
  through the Wi-Fi task and can wait.
- A frame slower than 120 ms is logged at once: `clock  : slow frame N ms = restore / sweep+ring+bars /
  text+time / flush`.
- Build: **2,958,379 bytes** (62%).

## v3.31 - home screen: app grid instead of the carousel

`preview_home_grid.png` (layout mock; the real icons are the existing ones). Owner feedback: sliding the
carousel and opening an app (double tap on the middle icon) was hard and counter-intuitive.

- **All 8 apps visible at once**, two rows of four under the Sons of Sudo sign, each with its name.
  **One tap opens.** The app lights up the moment the finger lands (ring + white name, a small patch) and
  opens on the lift; sliding more than 30 px away cancels.
- Rows: PICTURES / ASK DANNY / RUMOURS / SETTINGS, then RADAR / ORACLE / CLOCK / SLAP (the bottom row is
  narrower because the panel is round; the longest name sits in the top row). Every icon and label was
  checked against the round mask with the real font; the boot layout check now checks the grid.
- The wallpaper behind the grid is darkened to half brightness so the names read; the Wi-Fi / badge
  network line stays underneath.
- Removed: carousel slots and slide animation, page dots, name pill, the double-tap logic (v3.20), and
  swipe detection (home was the only screen using it).
- Build: **2,957,135 bytes** (62%).

## v3.32 - apps opening and closing again; grid 5% bigger

Owner feedback: some apps (the clock) were hard to start - sometimes it opened and closed again,
sometimes nothing seemed to happen. And the grid could be a bit bigger.

- **Cause**: a finger counts as lifted after `RELEASE_MS` (35 ms) without a touch report. While the badge
  is busy (drawing a clock frame, or an app loading for a few hundred ms) nobody polls the touch chip, and
  the CST9217 only reports when it has new data, so the first reads after such a gap are empty. That gap
  counted as a lift: one press became two. The first opened the app (on the lift); the second landed on
  the new screen, where a tap fires on the press - on the clock that means "home". If the app took long to
  load, all of this happened before it was ever shown: "nothing happened". Simulated: old code splits a
  press held across a 300 ms load into down/lift/down/lift; fixed code keeps it one press.
- **Fix**: only time actually spent polling counts toward a lift (`tLastSeen` moves forward by the blind
  gap). Plus: a press within `NEW_SCREEN_MS` (300 ms) after a tap changed the screen is ignored (logged as
  `same finger, ignored`), and the tap guard now starts when the tapped action has finished
  (`tapAndNote()`), not before a slow app load.
- **Grid**: icons r 30 -> 32, tap radius 44 -> 46, rows and labels moved to fit; checked against the round
  mask with the real font.
- Build: **2,957,379 bytes** (62%).

## v3.33 - Danny-gotchi

`preview_gotchi.png`, `preview_gotchi_sheet.png`. A pixel-art Danny (24 x 26 sprite drawn 6x: brown hair,
glasses, blue hoodie with a green `>_` prompt) who lives on the badge and needs looking after.

- **Needs** (0-100, shown as bars): COFFEE -8/h, MOOD -5/h, ENERGY -4/h awake; asleep COFFEE -3/h, MOOD -2/h,
  ENERGY +25/h (wakes up by himself when full). They run down in real time (internet time), also while the
  app is closed or the badge is off - on opening, the time since the last visit is applied (max 72 h).
- **Buttons**: COFFEE (+30; when already above 85 he gets WIRED, jittery and grumpy), THANKS (+25 mood,
  hearts, a reply: "It was DNS, by the way."), SLEEP / WAKE. Waking him with coffee or thanks: grumpy.
  Tap Danny: he waves. HOME pill as in Oracle.
- **Badge life**: every slap received since the last look costs 6 mood and wakes him ("Ouch! Slapped 3
  times!"); shaking the badge makes him dizzy ("Whoa... packet loss..."). Low needs blink red and he says
  so ("Need... coffee..."); otherwise a rotating quip.
- **KERNEL PANIC** when all three hit zero: grey Danny, red rack lights, uptime frozen; REBOOT starts a new
  one. Uptime gives a title: INTERN, JUNIOR ENG, MEDIOR ENG, SENIOR ENG, PRINCIPAL, ROOT.
- Retro square-wave beeps (made at start-up, played with `sosPlay` - the slap mute silences them).
- State in NVS namespace `gotchi`, saved on every action and every 5 minutes. Scene redrawn ~10 fps as a
  326 x 234 patch; the top bars only when they change (or blink).
- **Home grid**: GOTCHI replaces SETTINGS in the top row (Danny's head as the icon, with a red dot when he
  needs care); **SETTINGS is now a small gear** under the grid.
- Pixel art: `tools/make_gotchi_assets.py` -> `Badge/gotchi_assets.h` (palette indices + RGB565 palette,
  plus a grey palette for the crash).
- Build: **2,971,559 bytes** (62%).

## v3.34 - Oracle sounds

Owner: "the eight ball is without sound?" - it only played the slap swish at 60% volume.

- **Swirl** (1.45 s, starts with the spin): the liquid of a magic 8-ball - low-pass filtered noise whose
  cutoff rises and falls with the swirl, slosh waves that slow down, and bubbles (short upward-gliding
  sine blips, fewer as it settles). Shorter than the 1.6 s spin so the speaker is free for:
- **Reveal** (1.7 s, as the answer window opens): a bell arpeggio A5 - C#6 - E6 - A6 (inharmonic partials,
  exponential decay) with a little shimmer. If the swirl is still finishing, the Oracle waits up to 250 ms
  (`sosSoundBusy()`, new in Sos.ino) instead of skipping it.
- Synthesised by `tools/make_oracle_sounds.py` (no third-party audio) -> `Badge/oracle_sounds.h`, 98 kB,
  16 kHz int16, played with `sosPlay`. `--wav` writes listenable copies.
- The Settings mute row is now **"Sounds: ON/OFF"** ("slaps, oracle, gotchi - tap to mute"): it always
  silenced all three, but was labelled "Slap sound".
- Build: **3,072,371 bytes** (65%).

## v3.35 - Oracle sounds audible; sound log

Owner after v3.34: "not hear the sound".

- The v3.34 swirl sat mostly below 1 kHz (low-pass noise 250-1650 Hz, bubbles 350-800 Hz) - the badge's
  tiny speaker does very little down there. Now: noise 900-3500 Hz, bubbles 900-1700 Hz, and both sounds
  soft-compressed (tanh) and normalised to 92%: RMS 4000/5100 -> ~8400 (+6 dB), 91% / 100% of the energy
  above 800 Hz.
- `sosPlay()` now logs every sound: `sound  : playing 1.45 s, gain 256`, or why it was skipped (Sounds OFF
  in Settings / another sound still playing / no audio path). Same player as slaps, gotchi and the old
  clap voice, so if those are heard, the Oracle is too - unless Settings says Sounds: OFF.
- Build: **3,072,607 bytes** (65%).

## v3.36 - home screen: bigger icons and names, two pages

`preview_home_grid.png` (mock: letters stand in for the real icons). Owner: "text and icons on main screen
a bit bigger; if not enough room, use something like a next page".

- Icons r 32 -> **38** (+19%), names in the new **UiLabel** font (DejaVu Sans Bold, caps 11 instead of 9;
  `tools/make_ui_fonts.py`, the other fonts byte-identical).
- That no longer fits 8 apps under the sign, so the grid has **pages of 5**: three on top, two below (the
  panel narrows at the bottom; a lone bottom app is centred). Page 1: PICTURES, ASK DANNY, RUMOURS / GOTCHI,
  SLAP. Page 2: RADAR, ORACLE, CLOCK / SETTINGS - Settings is an ordinary app again (the v3.33 gear is gone).
- Change page: the round **arrow** beside the bottom row, or a clear sideways **swipe** (> 50 px, mostly
  horizontal). Page dots at the bottom. Opening an app is still one tap (lit on press, opens on lift).
- The Wi-Fi / badge network line is now a **pill at the top** of the screen (no room under the grid);
  in setup mode it reads "join ThanksDanny-setup".
- All icons, names, arrows and the pill checked against the round mask with the real fonts; the boot
  layout check covers the slots and arrows.
- Build: **3,075,295 bytes** (65%).

## v3.37 - Oracle quieter

Owner: make the 8-ball volume lower. `ORA_VOL` in `Oracle.ino`: swirl gain 256 -> 120, reveal 230 -> 108
(about half, -6.5 dB). Other sounds unchanged.

## v3.38 - phone: remote page, Bluetooth media remote (MUSIC), iPhone notifications

`preview_music_notify.png`. Three ways to link the badge to a phone:

**1. Remote page** - `http://badge-<name>.local/remote` (or the badge's IP + `/remote`; the setup page links
to it). Same Wi-Fi, no app, no password (like the setup page). Volume slider (codec 0-100, saved in NVS
`audio/vol`, `decSetVolume()`), Sounds on/off, soundboard (slap, drum roll, Oracle chime, random rumour),
open any app / home, ask the Oracle, slap another badge, Danny-gotchi coffee / thanks / sleep with his bars.
`Remote.ino` + `remote_page.h`. JSON API: `GET /api/state`, `GET /api/do?...`. Volume and mute act at once;
everything that draws or plays is queued and run by `remoteTick()` in loop() (so the 2 s Oracle animation
never runs inside a web request). Tested in headless Chromium against a mock API: no JS errors, every
button sends the right command. NOTE: the HTML lives in a header because Arduino's prototype generator
(ctags) cannot read a raw string literal in an .ino - with it in Remote.ino no prototypes were generated
at all and the whole sketch failed to compile.

**2. MUSIC app** - the badge as a Bluetooth LE **media remote**: a HID device with one consumer-control
report (next, previous, stop, play/pause, mute, volume up/down), so whatever app is playing on the phone
reacts. Screen: big play/pause, previous / next, volume down / mute / up, live status ("pair "SoS DANNY"
on your phone" / pairing / connected / notifications on), a Bluetooth on/off pill, HOME. Pairing is "just
works" with bonding and Secure Connections; NimBLE keeps up to 3 bonds in NVS. Bluetooth stays OFF until
MUSIC is opened once (NVS `phone/bt`), then starts at every boot. The ESP32-S3 has no Bluetooth Classic,
so the badge cannot be a Bluetooth speaker for the phone.

**3. iPhone notifications (ANCS)** - once a paired iPhone is connected and the link encrypted, the badge
(peripheral) acts as GATT client on the same connection: discovers Apple's Notification Center Service,
subscribes to Notification Source + Data Source, and for each new notification asks the Control Point for
app id, title (40) and message (120). A full-screen card (app pill coloured by category, title, wrapped
message; INCOMING CALL for calls), soft ping (start of the Oracle chime), 8 s (calls 15 s) or until tapped,
then back to the previous screen. UTF-8 -> ASCII (accents dropped, curly quotes straightened, emoji
skipped). App ids mapped to names for common apps. The advertisement solicits ANCS (AD 0x15). Android has
no ANCS: media keys only.
- The core's BLE library (NimBLE backend on the S3) is used for the HID part; its BLEClient refuses an
  existing connection, so ANCS talks to the NimBLE host directly (`ble_gattc_*`) and watches GAP events
  with `ble_gap_event_listener_register()`. All of it is `src/phonelink.cpp` with a primitive-typed API
  (`src/phonelink.h`), so the .ino auto-prototypes never see NimBLE types. UI in `Phone.ino`.
- Home grid page 2: RADAR, ORACLE, CLOCK / SETTINGS, MUSIC.
- NOT TESTED ON HARDWARE (no phone here): the log prefix `phone` says what happens - Bluetooth on (+ free
  internal RAM), connected, link encrypted, iPhone notifications on, each notification.
- Build: **3,334,935 bytes** (70%; Bluetooth adds ~250 kB), static RAM 28%.

## v3.39 - Claudio is now Claud-I/O

Display name only: `SOS_NAME[0] = "CLAUD-I/O"` - slap screen, slap pop-up, Settings, the remote page, the
Bluetooth name ("SoS CLAUD-I/O"), the screensaver ticker (`$CLAUD-I/O`, headline) and `web/poll.html`. The clock
HUD now shows the display name (it showed the id). The **id stays `claudio`**: MQTT login, topics, the ACL's
`%u`, `badge-claudio.local` and `tools/sos_cli.py` are unchanged - nothing to change on the broker.
Checked: the 9-character name fits under its slap button and in the clock HUD; "CLAUD-I/O SLAPPED YOU!" uses
the smaller size like other long lines. Build: 3,334,979 bytes (70%).

## v3.40 - fix: boot loop when Bluetooth was on

First run of v3.38+ on hardware: once MUSIC had been opened, every boot crashed right after the home screen
(`LoadProhibited`, backtrace `phoneBegin` -> `BLEHIDDevice::manufacturer(String)` -> `BLECharacteristic::setValue`).
In the core's BLE library `manufacturer()` *creates* the optional characteristic and `manufacturer(String)` only sets
it - called alone it writes through a null pointer. Now `sHid->manufacturer()->setValue("Sons of Sudo")`.
Safety net: NVS `phone`/`starting` is set just before Bluetooth starts and cleared once it is up; if it is still set
at boot, the last start crashed inside Bluetooth, so it stays off (log line `phone  : ! Bluetooth crashed...`) and
MUSIC switches it on again. A Bluetooth bug can no longer lock the badge in a boot loop.
Build: 3,335,079 bytes (70%).

## v3.41 - smoother: 4x faster screen updates, home pages follow the finger

A badge with Waveshare's stock firmware (LVGL) scrolled much more smoothly. Two causes on our side:

- **Slow flush.** `Arduino_ESP32QSPI::writePixels()` sends 1024-pixel pieces, and for each one byte-swaps
  from PSRAM, then starts the transfer and waits for it - nothing overlaps (~213 rounds, 46 ms a frame,
  ~1/4 of what 80 MHz quad SPI carries). New `src/fastlcd.cpp`: a second SPI device on the same bus with
  two 16 kB DMA buffers in internal RAM; the CPU swaps the next rows while the previous piece is on the
  wire (queued DMA). The address window still goes through the library (`Arduino_CO5300` caches it).
  The GFX bus is now created with `is_shared_interface = true` (otherwise the library holds the bus for
  ever). `FastCanvas` (a `Arduino_Canvas` whose `flush()` takes this path) and `flushRect()` use it -
  `flushRect()` no longer copies into a temp buffer (the fast path takes a row stride). Boot log:
  `panel  : fast flush on - full frame N ms (library path 46 ms)`. Falls back to the library if the
  device or buffers cannot be had.
- **No drag.** The home pages jumped after a 50 px swipe. Now a sideways press of 14 px becomes a drag:
  the background band and each page's icons are rendered once into PSRAM layers (key colour 0x0821 =
  see-through), every frame mixes them at the scroll offset and sends the 228-row band; past the first /
  last page it gives way at 1/3. On the lift: next page after a quarter-screen drag or a quick flick
  (>40 px in <250 ms), otherwise back; a 170 ms ease-out glide. Log: `ui     : home page 2/2 (drag ...
  frames)`.

Not changed yet: the tap guards (90 ms between taps, 300 ms after a screen change) - to be shortened once
the faster flush is confirmed on the badge. Build: 3,338,087 bytes (70%).

## v3.42 - colourful home tiles, a bit bigger

Like the stock Waveshare firmware: phone-style tiles - a rounded square (superellipse) with a colour
gradient, a soft sheen, a drop shadow and a white glyph - instead of dark 76 px discs with line icons.
84 px tiles (`GRID_R` 42); rows moved to y 282 / 386, labels 8 px under the tile, arrows y 386, dots y 457
(all checked against the round panel). Pre-rendered by `tools/make_home_icons.py` at 4x and scaled down
(anti-aliased), stored as RGB565 + 8-bit alpha in `Badge/home_icons.h` (92x92 each, ~250 kB flash) and
alpha-blended into the canvas by `drawAppIcon()`. Glyphs: Material Symbols Rounded (Google, Apache 2.0),
filled, subset to the 8 glyphs used: `assets/fonts/MaterialSymbolsRounded-HomeIcons.ttf` (3 kB, licence
next to it). Drawn by the tool: the Oracle 8-ball and Danny-gotchi's head (from `make_gotchi_assets.py`).
Pressed = the tile lightens with a white frame; Gotchi's red "needs you" dot sits on the tile corner.
Drag layers (v3.41) blend tile edges against a copy of the band (`iconBlendBg`), not the key colour, so no
dark halo while sliding. Preview: `preview_home_icons_screen.png`. Build: 3,588,811 bytes (76%).

## v3.43 - snappier page swipes

Swiping felt sluggish, worst right after a page change. Causes and fixes:
- Every drag started by drawing everything: take the press highlight off (full band redraw), then the
  background band and both pages of icons for the layers (~60-100 ms before anything moved). Now the layers
  are made ONCE, while the home screen sits idle (`homeDragWarm()` from loop, borrowing the canvas band and
  putting it back), and only made again when Gotchi's red dot changes. The press highlight is not redrawn
  away - the drag frames repaint the band; only the frame's top edge above the band is restored.
- The glide after the lift BLOCKED loop() for 170 ms and then redrew the whole band (another ~30 ms):
  a quick second swipe was half missed. The glide now runs from loop() (`homeGlideTick()`, 160 ms
  ease-out); a new touch lands it at once. At the end only the arrows and dots are drawn and sent.
- Each frame mixes only the part of each band row the round glass shows (~20% less work).
- Flick: >30 px in <250 ms (was 40).
Log: `ui     : drag layers made in N ms`, and per page change `... N frames, N ms/frame`.
Build: 3,589,823 bytes (76%).

## Next steps

- Flash v2 and read the boot log's `full frame` and `composed + flush` timings; confirm the panel
  is clean at 80 MHz.
- The CYD and the 3.5" ESP32-S3 board carry the rest of the kit — see `..\CYBERPUNK.md`.
