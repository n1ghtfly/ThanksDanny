#!/usr/bin/env python3
"""Convert the user's wallpapers to 466x466 baseline JPEGs for the AMOLED badge.

Source : C:\\Users\\brain\\Downloads\\ThanksDanny\\wallpaper
Output : <sketch>/data/wallpaper   (this is the folder that goes into the device's
                                    LittleFS image, so the list is what the device shows)

Two things matter for the target device:

  * 466x466 square - it is a square panel, so a portrait phone wallpaper must be cropped.
    BIAS below is where the square is taken from vertically: 0.0 = top edge, 1.0 = bottom
    edge. Phone wallpapers usually keep their subject above centre (~0.35), but a few want
    the bottom, hence per-image values rather than one global rule.
  * baseline JPEG only - TJpg_Decoder cannot read progressive JPEGs, and half of the
    originals are progressive. `progressive=False` is explicit here, and the run verifies
    what actually landed on disk at the end rather than trusting the save call.
"""

from pathlib import Path
from PIL import Image, ImageOps

SRC = Path(r"C:\Users\brain\Downloads\ThanksDanny\wallpaper")
OUT = Path(r"C:\Users\brain\Documents\Arduino\ThanksDanny\data\wallpaper")
SIZE = 466
QUALITY = 88

# file name -> vertical crop bias, chosen by looking at each image.
BIAS = {
    "son of sudo.jpg": 0.12,                                     # keep the billboard
    "wp7807389-cyberpunk-2077-phone-4k-wallpapers.jpg": 0.88,    # keep the rider, not the fog
}
DEFAULT_BIAS = 0.35


def convert(src: Path, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    made = []
    for f in sorted(src.glob("*.jpg")):
        im = ImageOps.exif_transpose(Image.open(f)).convert("RGB")
        side = min(im.width, im.height)
        bias = BIAS.get(f.name, DEFAULT_BIAS)
        left = (im.width - side) // 2
        top = int((im.height - side) * bias)
        sq = im.crop((left, top, left + side, top + side)).resize((SIZE, SIZE), Image.LANCZOS)
        sq = ImageOps.autocontrast(sq, cutoff=1)
        name = f.stem.replace(" ", "_").lower()[:40] + ".jpg"
        sq.save(out_dir / name, "JPEG", quality=QUALITY, optimize=True, progressive=False)
        made.append(out_dir / name)
        print(f"  {f.name}\n    {im.width}x{im.height} bias {bias}  ->  {name}  "
              f"{(out_dir / name).stat().st_size / 1024:.0f} kB")

    # Read back what is on disk: the device's decoder is the thing that has to cope.
    bad = [p.name for p in made
           if Image.open(p).size != (SIZE, SIZE)
           or Image.open(p).info.get("progressive")
           or Image.open(p).format != "JPEG"]
    print(f"\n  {len(made)} file(s), all {SIZE}x{SIZE} baseline: {not bad}")
    if bad:
        print("  PROBLEMS:", bad)


if __name__ == "__main__":
    convert(SRC, OUT)
