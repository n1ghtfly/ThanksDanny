#!/usr/bin/env python3
"""Build the LittleFS image that goes into the badge's flash filesystem, and verify it.

Why a tool instead of a command line: the image size has to match the partition exactly,
and an earlier hand-computed size was 128 KB too big - esptool refused it, which wasted a
flash cycle. Reading the offset and size straight out of the partition table removes the
arithmetic (and the mistake) entirely, and the unpack-and-compare step means "the device
will see the right files" is proven rather than assumed.

Layout it works against (FQBN PartitionScheme=app5M_little24M_32MB):
    app0     0x010000  4.5 MB
    spiffs   0x910000  23,986,176 bytes   <- this image
    coredump 0x1FF0000
"""

import csv
import filecmp
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CORE = Path(r"C:\Users\brain\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.12")
CSV = CORE / "tools" / "partitions" / "large_littlefs_32MB.csv"
TOOLS = Path(r"C:\Users\brain\AppData\Local\Arduino15\packages\esp32\tools\mklittlefs")
DATA = Path(r"C:\Users\brain\Documents\Arduino\ThanksDanny\data")
OUT = Path(r"C:\Users\brain\Documents\Arduino\ThanksDanny\build\wallpaper.littlefs.bin")
FLASH_BYTES = 32 * 1024 * 1024


def find_mklittlefs() -> Path:
    hits = sorted(TOOLS.glob("*/mklittlefs.exe"))
    if not hits:
        sys.exit("mklittlefs.exe not found - install the esp32 core's tools")
    return hits[-1]


def partition_for(subtype: str) -> tuple[int, int]:
    """(offset, size) of the named partition, read from the table the build actually uses."""
    rows = [r for r in csv.reader(CSV.read_text().splitlines()) if r and not r[0].startswith("#")]
    for r in rows:
        if r[1].strip() == "data" and r[2].strip() == subtype:
            return int(r[3], 16), int(r[4], 16)
    sys.exit(f"no '{subtype}' partition in {CSV.name}")


def main() -> None:
    offset, size = partition_for("spiffs")
    last = offset + size
    print(f"  partition : spiffs @ 0x{offset:06X}, {size} bytes ({size / 1024 / 1024:.2f} MB)")
    print(f"  fits      : last byte 0x{last:X} vs 32 MB flash 0x{FLASH_BYTES:X} -> {last <= FLASH_BYTES}")
    if last > FLASH_BYTES:
        sys.exit("  ! partition runs past the end of flash")

    mk = find_mklittlefs()
    print(f"  tool      : {mk}")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.unlink(missing_ok=True)

    pack = subprocess.run([str(mk), "-c", str(DATA), "-p", "256", "-b", "4096",
                           "-s", str(size), str(OUT)],
                          capture_output=True, text=True)
    for line in pack.stdout.splitlines():
        if line.strip():
            print(f"    packed {line.strip()}")
    if pack.returncode != 0:
        sys.exit(f"  ! mklittlefs failed: {pack.stderr.strip()}")

    written = OUT.stat().st_size
    print(f"  image     : {written} bytes  ({'matches partition' if written == size else 'WRONG SIZE'})")
    if written != size:
        sys.exit("  ! image size does not match the partition")

    # Unpack it and compare, so the claim about the device's contents rests on reading the
    # image back rather than on trusting the pack step.
    with tempfile.TemporaryDirectory() as td:
        # mklittlefs takes forward slashes; a raw Windows backslash path fails it (seen live).
        subprocess.run([str(mk), "-u", td.replace("\\", "/"), str(OUT)],
                       capture_output=True, text=True, check=True)
        src_files = sorted(p for p in DATA.rglob("*") if p.is_file())
        mismatches = []
        for src in src_files:
            rel = src.relative_to(DATA)
            dst = Path(td) / rel
            if not dst.exists() or not filecmp.cmp(src, dst, shallow=False):
                mismatches.append(str(rel))
        extra = [str(p.relative_to(td)) for p in Path(td).rglob("*")
                 if p.is_file() and not (DATA / p.relative_to(td)).exists()]
        print(f"  contents  : {len(src_files)} source file(s), "
              f"{len(mismatches)} mismatch, {len(extra)} unexpected")
        for m in mismatches:
            print(f"    MISMATCH: {m}")
        for e in extra:
            print(f"    UNEXPECTED: {e}")
        if mismatches or extra:
            sys.exit("  ! image contents do not match data/")

    total = sum(p.stat().st_size for p in DATA.rglob("*") if p.is_file())
    print(f"  VERDICT   : image matches data/ exactly ({total / 1024:.0f} kB of files, "
          f"{100 * total / size:.1f}% of the partition)")
    print(f"  flash it  : esptool -p COM13 write-flash 0x{offset:X} {OUT}")


if __name__ == "__main__":
    main()
