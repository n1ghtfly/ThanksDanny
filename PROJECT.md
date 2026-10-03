# Thanks Danny badge - project index (v2.7, 3 Oct 2026)

Firmware and tools for the Waveshare **ESP32-S3-Touch-AMOLED-1.75C** round badge.
`README.md` is the full engineering log (hardware facts, every version's reasoning and
measurements); this page is the map.

## What the badge does (v2.7)

- **Home**: Sons of Sudo wallpaper and an app carousel: one big icon, its neighbours at the sides,
  page dots, and a name + Wi-Fi pill. Swipe left/right to move, tap the middle icon to open it,
  tap a side icon to bring it to the middle. The icon lights up as soon as it is touched.
- **Pictures**: downloads the pictures from GitHub Pages
  (`https://n1ghtfly.github.io/danny-pics-7f3c9a/`) into flash, then shows them still, in random
  order, with a short slide between them. Tap = next, hold 0.7 s = home.
- **Decider**: a coin toss to a drum roll, landing with a cymbal crash on *Approved by Danny* or
  *Disapproved by Danny* (50/50). Then AGAIN | HOME.
- **Settings**: Wi-Fi setup on the phone (the badge raises its own `ThanksDanny-setup`
  network), touch test, close.
- **BOOT button**: back to the home screen from anywhere.

## Layout

| Folder / file | What it is |
|---|---|
| `Badge/` | **The firmware.** `Badge.ino` (screen, touch, carousel, Wi-Fi), `Gallery.ino` (Pictures), `Decider.ino` (coin toss + sound), `decider_assets.h` / `decider_sound.h` (generated), `es8311.*` (audio codec driver), `sketch.yaml` (board settings) |
| `tools/flash_badge.cmd` | Double-click: clean build + upload + boot log. |
| `tools/prep_decider.py` | `assets/chooser` badges -> `Badge/decider_assets.h` |
| `tools/prep_decider_sound.py` | `assets/chooser/drummroll.mp3` -> `Badge/decider_sound.h` (needs ffmpeg) |
| `tools/prep_wallpapers.py`, `make_fs_image.py` | wallpapers -> `data/wallpaper` -> `build/wallpaper.littlefs.bin` |
| `tools/prep_github_pics.py` | pictures for the GitHub Pages gallery -> `github-pics/` |
| `data/wallpaper/` | the 466x466 wallpapers that are on the badge's filesystem |
| `build/wallpaper.littlefs.bin` | that filesystem image, ready to flash |
| `github-pics/` | contents of the GitHub Pages repo (`git-remote-config.txt` has its address) |
| `assets/chooser/` | originals: `positive.png`, `negative.jpg`, `drummroll.mp3` |
| `assets/wallpaper_originals/` | the original wallpaper JPEGs |
| `backups/` | `Badge_v1/` (first Badge version), `factory_head_8MB.bin` (first 8 MB of the factory flash) |
| `Bringup/`, `BlitTest/`, `AudioProbe/`, `Wallpaper/`, `reference/` | the bring-up and test sketches that proved each part of the board |
| `preview_*.png`, `drumroll_as_on_badge.wav` | renders through the round mask, and the sound exactly as played |

## Build environment (what v2.7 was compiled and tested with)

- Arduino IDE 2 (its built-in arduino-cli), **esp32 core 3.3.x** (3.3.12 used here)
- Libraries: **GFX Library for Arduino 1.6.8**, **SensorLib 0.5.0**, **TJpg_Decoder 1.1.0**
- Board settings: in `Badge/sketch.yaml`; Partition Scheme **32M Flash (4.8MB APP/22MB LittleFS)**
- v2.7 build: 2,481,179 bytes (52% of the app partition), globals 17%

## Restore from scratch (new PC, or a wiped board)

1. Put this `ThanksDanny` folder in `Documents\Arduino\`, install the core and libraries above.
2. Flash the filesystem once (wallpaper; the pictures re-download themselves):
   `esptool -p COM13 -c auto write-flash 0x910000 build\wallpaper.littlefs.bin`
3. Double-click `tools\flash_badge.cmd`. First boot with no Wi-Fi saved: join
   `ThanksDanny-setup` on a phone, open `http://192.168.4.1`, pick the network, type the password.

## On GitHub

Published at **https://github.com/n1ghtfly/ThanksDanny** (public) by `push_to_github.cmd` in
this folder - run it again after changes to commit and push them. Kept off GitHub on purpose
(see `.gitignore`):

- `build/` and every `*.bin`: the 24 MB filesystem image (rebuild it with
  `tools\prep_wallpapers.py` + `tools\make_fs_image.py`) and the factory flash backup.
- The downloaded `wp*.jpg` wallpapers and `preview_wallpapers.png`: third-party images, not
  ours to republish. Only the Sons of Sudo wallpaper is published.
- The badge's MAC address and the home network name/address were removed from the docs.
- `github-pics/` is a **submodule** pointing to the pictures repository
  (`n1ghtfly/danny-pics-7f3c9a`); clone with `git clone --recursive` to get it too.
