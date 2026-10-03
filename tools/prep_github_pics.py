#!/usr/bin/env python3
"""
Prepare pictures for the badge's GitHub-hosted slideshow.

Why this exists
  The badge's decoder (TJpg_Decoder) cannot read PROGRESSIVE JPEGs - it fails silently and
  just draws nothing. Anything published for the badge must therefore be re-encoded as
  BASELINE, and this script is what guarantees that rather than trusting the source files.

  It also sizes each picture for the round 466x466 panel: the SHORT side is scaled to 466
  and the long side is capped, so a landscape picture stays wider than the panel and the
  badge can slide/pan across it instead of showing a letterboxed strip.

Usage
  python prep_github_pics.py                 # uses the paths below
  python prep_github_pics.py --src DIR --out DIR

Output: numbered baseline JPEGs plus manifest.json, ready to commit to a web host.
"""

import argparse
import json
import os
import sys

from PIL import Image

DEFAULT_SRC = r"C:\Users\brain\Downloads\ThanksDanny\pics"
DEFAULT_OUT = r"C:\Users\brain\Documents\Arduino\ThanksDanny\github-pics"

SHORT_SIDE = 466          # the panel's visible diameter - nothing smaller is worth it
MAX_LONG_SIDE = 1100      # cap decode time; the badge blits ~0.5 Mpx per frame at best
QUALITY = 85


def is_baseline_jpeg(path):
    """True only for baseline (non-progressive) JPEG."""
    with Image.open(path) as im:
        if im.format != "JPEG":
            return False
        # PIL exposes 'progressive' in the raw decoder config
        return not bool(im.info.get("progressive", 0))


def prep(src, out):
    if not os.path.isdir(src):
        sys.exit(f"source folder not found: {src}")
    os.makedirs(out, exist_ok=True)

    files = sorted(f for f in os.listdir(src)
                   if f.lower().endswith((".jpg", ".jpeg", ".png")))
    if not files:
        sys.exit(f"no pictures in {src}")

    entries = []
    print(f"{len(files)} source picture(s) from {src}\n")
    for i, name in enumerate(files, start=1):
        p = os.path.join(src, name)
        im = Image.open(p)
        w0, h0 = im.size

        # Flatten any alpha onto black; the panel has no transparency.
        if im.mode in ("RGBA", "LA", "P"):
            im = im.convert("RGBA")
            flat = Image.new("RGB", im.size, (0, 0, 0))
            flat.paste(im, mask=im.split()[-1])
            im = flat
        else:
            im = im.convert("RGB")

        # Scale the SHORT side to the panel diameter, then cap the long side.
        scale = SHORT_SIDE / min(im.size)
        nw, nh = round(im.width * scale), round(im.height * scale)
        if max(nw, nh) > MAX_LONG_SIDE:
            shrink = MAX_LONG_SIDE / max(nw, nh)
            nw, nh = round(nw * shrink), round(nh * shrink)
        im = im.resize((nw, nh), Image.LANCZOS)

        out_name = f"p{i:02d}.jpg"
        out_path = os.path.join(out, out_name)
        im.save(out_path, "JPEG", quality=QUALITY, optimize=True, progressive=False)

        # Read it back off disk and prove it: baseline, right size, decodable.
        with Image.open(out_path) as check:
            check.load()
            cw, ch = check.size
            baseline = not bool(check.info.get("progressive", 0))
        if not baseline:
            sys.exit(f"FATAL: {out_name} came out progressive - the badge cannot read it")
        if (cw, ch) != (nw, nh):
            sys.exit(f"FATAL: {out_name} is {cw}x{ch}, expected {nw}x{nh}")

        kb = os.path.getsize(out_path) // 1024
        entries.append({
            "file": out_name,
            "w": cw,
            "h": ch,
            "bytes": os.path.getsize(out_path),
            "src": name,
        })
        print(f"  {name:42s} {w0}x{h0}  ->  {out_name}  {cw}x{ch}  {kb} kB  baseline")

    manifest = {"panel": 466, "count": len(entries), "pictures": entries}
    man_path = os.path.join(out, "manifest.json")
    with open(man_path, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)

    # The badge has no JSON parser (and adding one costs flash for nothing), so it reads this
    # instead: one "<name> <bytes> <width> <height>" per line. The badge needs the width to
    # size its PSRAM frame before decoding, and the byte count to know whether a cached copy
    # is already current. The JSON stays for the human-browsable page.
    list_path = os.path.join(out, "list.txt")
    with open(list_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f"# {len(entries)} pictures, panel {SHORT_SIDE}\n")
        fh.write("# <file> <bytes> <width> <height>\n")
        for e in entries:
            fh.write(f"{e['file']} {e['bytes']} {e['w']} {e['h']}\n")

    total = sum(e["bytes"] for e in entries)
    print(f"\n{len(entries)} picture(s), {total // 1024} kB total")
    print(f"manifest: {man_path}")
    print(f"list    : {list_path}  (what the badge reads)")
    print("all baseline: the badge can decode every one")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--out", default=DEFAULT_OUT)
    a = ap.parse_args()
    prep(a.src, a.out)
