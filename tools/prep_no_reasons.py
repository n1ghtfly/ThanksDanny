#!/usr/bin/env python3
"""Decider excuses: turn No-as-a-Service's reasons.json into the badge's no/reasons.txt.

When the Decider lands on DISAPPROVED, the badge shows one of these as Danny's "OFFICIAL REASON".
Source: https://github.com/hotheadhacker/no-as-a-service (MIT licence, (c) 2026 hotheadhacker) -
the whole list is copied once onto our own GitHub Pages site, so the badge never calls their API
(no extra handshake, works offline, no rate limit).

The badge's built-in font is plain ASCII, so curly quotes and dashes are straightened and
anything else non-ASCII drops the line. Lines that would not fit the card (4 lines of 24
characters at text size 2, wrapped at spaces only - as the badge does) are left out.

Output, in github-pics/no/ (publish with push_pictures_site.cmd or update_badge_content.cmd):
    reasons.txt   one reason per line; lines starting with # are comments
    index.txt     the byte size of reasons.txt - the badge only downloads when it changed

Usage:  python prep_no_reasons.py                   (downloads reasons.json from GitHub)
        python prep_no_reasons.py path/to/reasons.json [out_dir]
"""
import json, sys, textwrap, urllib.request
from pathlib import Path

URL = "https://raw.githubusercontent.com/hotheadhacker/no-as-a-service/main/reasons.json"
OUT = Path(__file__).resolve().parent.parent / "github-pics" / "no"
COLS, ROWS = 24, 4                     # must match NOPE_COLS / NOPE_ROWS in Badge/Nope.ino

FIX = {"‘": "'", "’": "'", "“": '"', "”": '"', "—": " - ",
       "–": "-", "…": "...", " ": " "}


def clean(s):
    for a, b in FIX.items():
        s = s.replace(a, b)
    s = " ".join(s.split())
    return s if all(32 <= ord(c) < 127 for c in s) else None


def main():
    if len(sys.argv) > 1:
        reasons = json.load(open(sys.argv[1], encoding="utf-8"))
    else:
        print(f"downloading {URL}")
        with urllib.request.urlopen(URL, timeout=30) as r:
            reasons = json.loads(r.read().decode("utf-8"))
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else OUT
    out.mkdir(parents=True, exist_ok=True)

    kept, seen, dropped = [], set(), 0
    for raw in reasons:
        s = clean(raw)
        if not s or s in seen or len(textwrap.wrap(s, COLS, break_on_hyphens=False)) > ROWS:
            dropped += 1
            continue
        seen.add(s)
        kept.append(s)

    body = "# Danny's official reasons for saying no. From hotheadhacker/no-as-a-service (MIT).\n"
    body += "\n".join(kept) + "\n"
    data = body.encode("ascii")
    (out / "reasons.txt").write_bytes(data)
    (out / "index.txt").write_text(f"{len(data)}\n", encoding="ascii", newline="\n")
    print(f"{len(kept)} reasons kept, {dropped} left out (too long, non-ASCII or duplicate)")
    print(f"{out / 'reasons.txt'}  {len(data) // 1024} kB")


if __name__ == "__main__":
    main()
