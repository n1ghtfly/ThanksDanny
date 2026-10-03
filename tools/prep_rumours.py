#!/usr/bin/env python3
"""Rumours: turn the MP3s in Downloads\\ThanksDanny\\mp3 into the badge's rumour set on the
GitHub Pages site (github-pics/rumours/), then run tools\\push_pictures_site.cmd to publish.

Each clip is re-encoded to 16 kHz mono MP3 (48 kbit/s): the badge's audio path already runs at
16 kHz for the Decider, so no clock change is needed between apps, the files are ~2.7x smaller
than the 128 kbit/s originals, and loudness is evened out so no rumour is much louder than the
next. Needs ffmpeg on PATH.

The speaker's name comes from the [brackets] at the start of each file name, tidied by NAMES.
Output list (rumours/rumours.txt), one line per clip - no JSON, the badge has no parser:
    <file> <bytes> <speaker name, may contain spaces>
"""
import re, subprocess, sys
from pathlib import Path

SRC = Path(r"C:\Users\brain\Downloads\ThanksDanny\mp3")
OUT = Path(r"C:\Users\brain\Documents\Arduino\ThanksDanny\github-pics\rumours")
if len(sys.argv) == 3:
    SRC, OUT = Path(sys.argv[1]), Path(sys.argv[2])

# Bracket text (lower-cased, trimmed) -> name shown on the badge. Anything not listed is
# title-cased as it is.
NAMES = {
    "barack obama": "Barack Obama",
    "donald trump": "Donald Trump",
    "emmanuel macron": "Emmanuel Macron",
    "gordon ramsay": "Gordon Ramsay",
    "king charles uk 111": "King Charles III",
    "meloni": "Giorgia Meloni",
    "morgan freeman": "Morgan Freeman",
    "peter griffin": "Peter Griffin",
    "pope leo xiv": "Pope Leo XIV",
}


def speaker(name: str) -> str:
    m = re.match(r"\s*\[([^\]]+)\]", name)
    raw = (m.group(1) if m else Path(name).stem).strip()
    key = re.sub(r"\s+", " ", raw.lower())
    return NAMES.get(key, raw.title())[:24]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    srcs = sorted(SRC.glob("*.mp3"), key=lambda p: speaker(p.name))
    lines = ["# Rumours for the Thanks Danny badge - AI-generated voice parody, not real statements",
             "# <file> <bytes> <speaker>"]
    for old in OUT.glob("r*.mp3"):
        old.unlink()
    for n, src in enumerate(srcs, 1):
        dst = OUT / f"r{n:02d}.mp3"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(src),
                        "-map_metadata", "-1", "-vn", "-ac", "1", "-ar", "16000",
                        "-af", "loudnorm=I=-16:TP=-1.5:LRA=11",
                        "-codec:a", "libmp3lame", "-b:a", "48k",
                        "-id3v2_version", "0", "-write_xing", "0", str(dst)], check=True)
        who = speaker(src.name)
        size = dst.stat().st_size
        lines.append(f"{dst.name} {size} {who}")
        print(f"  {dst.name}  {size / 1024:5.0f} kB  {who:18s} <- {src.name}")
    (OUT / "rumours.txt").write_text("\n".join(lines) + "\n", newline="\n")
    print(f"  {len(srcs)} rumour(s) -> {OUT / 'rumours.txt'}")


if __name__ == "__main__":
    main()
