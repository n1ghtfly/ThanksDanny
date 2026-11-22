#!/usr/bin/env python3
"""Danny-gotchi pixel art (v3.33) -> Badge/gotchi_assets.h

    python tools/make_gotchi_assets.py            # writes the header
    python tools/make_gotchi_assets.py --preview  # also writes preview_gotchi_sheet.png

Danny is a 24 x 26 sprite: brown hair, glasses, a blue hoodie with a green terminal prompt (>_) on the
chest. Expressions are the same body with different eyes / mouth / props, composed here from small
parts so they stay consistent. Everything is stored as palette indices (one byte per pixel) plus one
RGB565 palette; the badge draws each pixel as a SCALE x SCALE block. A grey palette is made for the
KERNEL PANIC (dead) state.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "Badge" / "gotchi_assets.h"
W, H = 24, 26

PAL = [  # index: (name, rgb)
    ("clear", (0, 0, 0)),
    ("outline", (22, 20, 30)),
    ("skin", (241, 196, 152)),
    ("skin2", (206, 152, 112)),
    ("hair", (96, 60, 30)),
    ("hair2", (138, 90, 46)),
    ("lens", (226, 236, 255)),
    ("pupil", (22, 20, 30)),
    ("hood", (44, 96, 178)),
    ("hood2", (30, 66, 130)),
    ("term", (90, 255, 130)),
    ("pants", (52, 54, 66)),
    ("mouth", (160, 46, 56)),
    ("cheek", (240, 120, 130)),
    ("mug", (240, 240, 236)),
    ("coffee", (112, 62, 28)),
    ("steam", (170, 176, 190)),
    ("heart", (236, 52, 84)),
    ("rack", (70, 76, 92)),
    ("rack2", (40, 44, 56)),
    ("shoe", (30, 28, 34)),
    ("moon", (250, 220, 110)),
    ("teeth", (255, 255, 255)),
]
IDX = {n: i for i, (n, _) in enumerate(PAL)}
CH = {".": "clear", "K": "outline", "S": "skin", "s": "skin2", "H": "hair", "h": "hair2", "W": "lens",
      "E": "pupil", "B": "hood", "b": "hood2", "T": "term", "P": "pants", "M": "mouth", "R": "cheek",
      "C": "mug", "c": "coffee", "~": "steam", "r": "heart", "G": "rack", "g": "rack2", "X": "shoe",
      "Y": "moon", "w": "teeth"}

# Body: head with glasses (lens interior rows 6-8, cols 7-9 left and 14-16 right), mouth rows 10-11,
# hoodie rows 15-22 with a >_ prompt, legs and shoes.
BODY = [
    "........HHHHHHHH........",  # 0
    "......HHHhhhHHHHHH......",  # 1
    ".....HHHHHHHHHHHHHH.....",  # 2
    ".....HHSSSSSSSSSSHH.....",  # 3
    ".....HSSSSSSSSSSSSH.....",  # 4
    ".....SKKKKKSSKKKKKS.....",  # 5  glasses top
    ".....KKWWWKKKKWWWKK.....",  # 6
    ".....SKWWWKSSKWWWKS.....",  # 7
    ".....SKWWWKSSKWWWKS.....",  # 8
    ".....SKKKKKSSKKKKKS.....",  # 9  glasses bottom
    ".....SSSSSSSSSSSSSS.....",  # 10
    "......SSSSSSSSSSSS......",  # 11
    ".......SSSSSSSSSS.......",  # 12
    "........sSSSSSSs........",  # 13
    "..........ssss..........",  # 14 neck
    ".......BBBbssbBBB.......",  # 15
    ".....BBBBBBbbBBBBBB.....",  # 16
    "....BBBBBBBBBBBBBBBB....",  # 17
    "....bBBBBTBBBBBBBBBb....",  # 18 prompt >
    "....bBBBBBTBBBBBBBBb....",  # 19
    "....SbBBBTBTTBBBBBbS....",  # 20 hands, prompt > _
    "....SbBBBBBBBBBBBBbS....",  # 21
    "......bBBBBBBBBBBb......",  # 22
    ".......PPPPPPPPPP.......",  # 23
    ".......PPPP..PPPP.......",  # 24
    "......XXXXX..XXXXX......",  # 25
]

# Eyes: list of (row, col, char) put inside the lenses. Lenses: rows 6-8, cols 7-9 / 14-16.
def both(cells):
    return cells + [(r, c + 7, ch) for r, c, ch in cells]

EYES = {
    "open":   both([(7, 8, "E")]),
    "blink":  both([(r, c, "S") for r in (6, 7, 8) for c in (7, 8, 9)] + [(7, 7, "E"), (7, 8, "E"), (7, 9, "E")]),
    "happy":  both([(7, 8, "E"), (8, 7, "E"), (8, 9, "E")]),           # ^ ^
    "sad":    both([(8, 8, "E")]),                                     # looking down
    "closed": both([(r, c, "S") for r in (6, 7, 8) for c in (7, 8, 9)] + [(8, 7, "E"), (8, 8, "E"), (8, 9, "E")]),
    "dizzy":  both([(6, 7, "E"), (6, 8, "E"), (6, 9, "E"), (7, 7, "E"), (7, 9, "E"), (8, 7, "E"), (8, 8, "E"), (8, 9, "E")]),
    "dead":   both([(6, 7, "E"), (6, 9, "E"), (7, 8, "E"), (8, 7, "E"), (8, 9, "E")]),   # X X
    "wide":   both([(6, 8, "E"), (7, 7, "E"), (7, 8, "E"), (7, 9, "E"), (8, 8, "E")]),   # big pupils
}
MOUTH = {
    "smile":  [(10, 9, "M"), (10, 14, "M"), (11, 10, "M"), (11, 11, "M"), (11, 12, "M"), (11, 13, "M")],
    "grin":   [(10, 9, "M"), (10, 10, "w"), (10, 11, "w"), (10, 12, "w"), (10, 13, "w"), (10, 14, "M"),
               (11, 10, "M"), (11, 11, "M"), (11, 12, "M"), (11, 13, "M")],
    "flat":   [(11, 10, "M"), (11, 11, "M"), (11, 12, "M"), (11, 13, "M")],
    "frown":  [(10, 10, "M"), (10, 11, "M"), (10, 12, "M"), (10, 13, "M"), (11, 9, "M"), (11, 14, "M")],
    "o":      [(10, 11, "M"), (10, 12, "M"), (11, 11, "M"), (11, 12, "M")],
    "zigzag": [(10, 9, "M"), (11, 10, "M"), (10, 11, "M"), (11, 12, "M"), (10, 13, "M"), (11, 14, "M")],
}
CHEEKS = [(10, 6, "R"), (10, 17, "R")]
MUG_FULL = [(r, c, ch) for r, row in zip(range(15, 22), [
    "~.~.",
    ".~.~",
    "CCCC.",
    "CccCC",
    "CCCCC",
    "CCCC.",
    "CCCC.",
]) for c, ch in zip(range(19, 24), row) if ch != "."]
MUG_EMPTY = [(r, c, "C") for r, c, ch in MUG_FULL if ch in "Cc" and r >= 17] + [(18, 20, "K"), (18, 21, "K")]

FRAMES = [  # name, eyes, mouth, cheeks, mug
    ("NORMAL", "open", "smile", False, None),
    ("BLINK", "blink", "smile", False, None),
    ("HAPPY", "happy", "grin", True, None),
    ("SAD", "sad", "frown", False, None),
    ("SLEEP", "closed", "o", False, None),
    ("DIZZY", "dizzy", "o", False, None),
    ("DEAD", "dead", "flat", False, None),
    ("COFFEE", "happy", "smile", True, MUG_FULL),
    ("HUNGRY", "sad", "frown", False, MUG_EMPTY),
    ("WIRED", "wide", "zigzag", False, MUG_FULL),
]

HEART = [".rr.rr.", "rrrrrrr", "rrrrrrr", ".rrrrr.", "..rrr..", "...r..."]
RACK = ["gggggggggg", "gGGGGGGGGg", "gGggggggGg", "gGggggggGg", "gGGGGGGGGg", "gGggggggGg", "gGggggggGg",
        "gGGGGGGGGg", "gGggggggGg", "gGggggggGg", "gGGGGGGGGg", "gGggggggGg", "gGggggggGg", "gGGGGGGGGg",
        "gGggggggGg", "gGggggggGg", "gGGGGGGGGg", "gggggggggg", "gg......gg", "gg......gg"]
ICON_MUG = ["..~..~....", ".~..~.....", "..........", "CCCCCCC...", "CcccccCCC.", "CCCCCCC.CC",
            "CCCCCCC.CC", "CCCCCCCCC.", "CCCCCCC...", ".CCCCC...."]
ICON_HEART = ["..........", ".rr...rr..", "rrrr.rrrr.", "rrrrrrrrr.", "rrrrrrrrr.", ".rrrrrrr..",
              "..rrrrr...", "...rrr....", "....r.....", ".........."]
ICON_MOON = ["...YYY....", "..YYY.....", ".YYY......", ".YYY......", "YYYY......", "YYYY......",
             ".YYYY....Y", ".YYYYY.YY.", "..YYYYYYY.", "...YYYYY.."]


def grid(rows):
    return [[IDX[CH[ch]] for ch in row] for row in rows]


def frame(eyes, mouth, cheeks, mug):
    g = grid(BODY)
    for r, c, ch in EYES[eyes] + MOUTH[mouth] + (CHEEKS if cheeks else []) + (mug or []):
        g[r][c] = IDX[CH[ch]]
    if mug:                                       # the right hand holds the mug: move it in
        g[20][19] = IDX["skin"]
    return g


def rgb565(c):
    r, g, b = c
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def grey(c):
    v = int(0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2])
    return (v * 3 // 4, v * 3 // 4, v * 3 // 4 + 10)


def emit(name, g, L):
    flat = [v for row in g for v in row]
    L.append(f"static const uint8_t {name}[{len(flat)}] PROGMEM = {{")
    for i in range(0, len(flat), len(g[0])):
        L.append("  " + ",".join(str(v) for v in flat[i:i + len(g[0])]) + ",")
    L.append("};")


def main():
    L = ["// Generated by tools/make_gotchi_assets.py - do not edit by hand.", "#pragma once", "#include <stdint.h>", "",
         f"#define GOT_W {W}", f"#define GOT_H {H}", f"#define GOT_NPAL {len(PAL)}"]
    L.append("static const uint16_t GOT_PAL[GOT_NPAL] = { " + ", ".join(f"0x{rgb565(c):04X}" for _, c in PAL) + " };")
    L.append("static const uint16_t GOT_PAL_DEAD[GOT_NPAL] = { " + ", ".join(f"0x{rgb565(grey(c)):04X}" for _, c in PAL) + " };")
    for i, (name, *_) in enumerate(FRAMES):
        L.append(f"#define GOT_{name} {i}")
    L.append(f"#define GOT_NFRAMES {len(FRAMES)}")
    frames = [frame(e, m, ch, mug) for _, e, m, ch, mug in FRAMES]
    for (name, *_), g in zip(FRAMES, frames):
        emit(f"GOT_F_{name}", g, L)
    L.append("static const uint8_t *const GOT_FRAMES[GOT_NFRAMES] = { " + ", ".join(f"GOT_F_{n}" for n, *_ in FRAMES) + " };")
    for name, rows in (("GOT_HEART", HEART), ("GOT_RACK", RACK), ("GOT_I_MUG", ICON_MUG), ("GOT_I_HEART", ICON_HEART), ("GOT_I_MOON", ICON_MOON)):
        L.append(f"#define {name}_W {len(rows[0])}\n#define {name}_H {len(rows)}")
        emit(name, grid(rows), L)
    OUT.write_text("\n".join(L) + "\n", encoding="ascii")
    print(f"wrote {OUT}")
    if "--preview" in sys.argv:
        from PIL import Image
        S = 6
        sheet = Image.new("RGB", (len(frames) * (W * S + 10) + 10, H * S + 20), (8, 10, 24))
        for k, g in enumerate(frames):
            pal = [c for _, c in PAL] if FRAMES[k][0] != "DEAD" else [grey(c) for _, c in PAL]
            for y in range(H):
                for x in range(W):
                    if g[y][x]:
                        for dy in range(S):
                            for dx in range(S):
                                sheet.putpixel((10 + k * (W * S + 10) + x * S + dx, 10 + y * S + dy), pal[g[y][x]])
        sheet.save(ROOT / "preview_gotchi_sheet.png")
        print("wrote preview_gotchi_sheet.png")


if __name__ == "__main__":
    main()
