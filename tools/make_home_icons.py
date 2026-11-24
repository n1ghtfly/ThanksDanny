#!/usr/bin/env python3
"""Home-screen app icons (v3.42) -> Badge/home_icons.h

    python tools/make_home_icons.py            # writes the header
    python tools/make_home_icons.py --preview  # also writes preview_home_icons.png

Phone-style tiles like Waveshare's stock firmware: a rounded square (superellipse) with a colour
gradient, a soft drop shadow and a white glyph. Rendered here at 4x and scaled down, so edges are
anti-aliased; stored as RGB565 + an 8-bit alpha per pixel. The badge blends them over the wallpaper.

Glyphs: Material Symbols Rounded (Google, Apache 2.0), filled, subset to the 8 glyphs used:
assets/fonts/MaterialSymbolsRounded-HomeIcons.ttf (licence next to it). The Oracle 8-ball and the
Danny-gotchi head (from tools/make_gotchi_assets.py) are drawn here.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "Badge" / "home_icons.h"
SYM = ROOT / "assets" / "fonts" / "MaterialSymbolsRounded-HomeIcons.ttf"
DEJAVU = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

TILE = 84          # tile size on the badge (GRID_R 42)
CAN = 92           # bitmap size: tile + room for the shadow
OX, OY = 4, 2      # tile offset in the bitmap
SS = 4             # supersampling

# order = APP_* indices in Badge.ino
APPS = [  # name, top colour, bottom colour, glyph (codepoint or special), glyph colour
    ("PICTURES", (255, 184, 76), (255, 84, 104), 0xE413, (255, 255, 255)),
    ("DECIDER",  (102, 226, 120), (22, 150, 72), 0xF577, (255, 255, 255)),
    ("RUMOURS",  (206, 120, 240), (112, 44, 230), 0xEF49, (255, 255, 255)),
    ("SLAP",     (255, 118, 104), (214, 22, 44), 0xE764, (255, 255, 255)),
    ("RADAR",    (64, 226, 200), (10, 120, 140), 0xF04E, (255, 255, 255)),
    ("ORACLE",   (84, 104, 200), (18, 24, 74), "8ball", None),
    ("CLOCK",    (70, 70, 78), (14, 14, 18), 0xEFD6, (255, 168, 40)),
    ("SETTINGS", (176, 186, 198), (92, 102, 116), 0xE8B8, (255, 255, 255)),
    ("GOTCHI",   (255, 230, 96), (255, 156, 64), "gotchi", None),
    ("MUSIC",    (255, 104, 150), (222, 20, 96), 0xE405, (255, 255, 255)),
]


def squircle_mask(size, n=4.6):
    """Superellipse |x|^n + |y|^n <= 1 at size x size, anti-aliased by the caller's downscale."""
    m = Image.new("L", (size, size), 0)
    px = m.load()
    h = size / 2
    for y in range(size):
        ny = abs((y + 0.5 - h) / h)
        for x in range(size):
            nx = abs((x + 0.5 - h) / h)
            if nx ** n + ny ** n <= 1:
                px[x, y] = 255
    return m


def gradient(size, top, bot):
    g = Image.new("RGB", (size, size))
    d = ImageDraw.Draw(g)
    for y in range(size):
        t = y / (size - 1)
        d.line([(0, y), (size, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return g


def glyph_layer(size, cp, colour):
    """A centred Material Symbol, ~56% of the tile, with a faint shadow."""
    font = ImageFont.truetype(str(SYM), int(size * 0.66))
    ch = chr(cp)
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    bbox = font.getbbox(ch)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x, y = (size - w) / 2 - bbox[0], (size - h) / 2 - bbox[1]
    sh = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(sh).text((x, y + size * 0.02), ch, font=font, fill=(0, 0, 0, 70))
    sh = sh.filter(ImageFilter.GaussianBlur(size * 0.015))
    layer.alpha_composite(sh)
    ImageDraw.Draw(layer).text((x, y), ch, font=font, fill=colour + (255,))
    return layer


def eightball(size):
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    c, r = size / 2, size * 0.34
    d.ellipse([c - r, c - r + size * 0.03, c + r, c + r + size * 0.03], fill=(0, 0, 0, 90))   # shadow
    for i in range(40):                       # ball: black with a soft light from the top left
        t = i / 39
        rr = r * (1 - t * 0.55)
        k = int(18 + 60 * t)
        off = r * 0.28 * t
        d.ellipse([c - off - rr, c - off - rr, c - off + rr, c - off + rr], fill=(k, k, k + 6, 255))
    wr = r * 0.46
    wx, wy = c - r * 0.06, c - r * 0.08
    d.ellipse([wx - wr, wy - wr, wx + wr, wy + wr], fill=(248, 248, 252, 255))
    f = ImageFont.truetype(DEJAVU, int(wr * 1.45))
    d.text((wx, wy + wr * 0.04), "8", font=f, fill=(16, 16, 20, 255), anchor="mm")
    return layer


def gotchi(size):
    sys.path.insert(0, str(ROOT / "tools"))
    import make_gotchi_assets as g
    fr = g.frame("happy", "grin", True, None)
    rows, cols = range(0, 15), range(4, 20)          # the head
    s = size * 0.6 / len(cols)
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0 = (size - s * len(cols)) / 2
    y0 = (size - s * len(rows)) / 2 + size * 0.02
    for j, r in enumerate(rows):
        for i, c in enumerate(cols):
            idx = fr[r][c]
            if idx == 0:
                continue
            col = g.PAL[idx][1]
            d.rectangle([x0 + i * s, y0 + j * s, x0 + (i + 1) * s - 1, y0 + (j + 1) * s - 1], fill=col + (255,))
    return layer


def render(top, bot, glyph, gcol):
    T, C = TILE * SS, CAN * SS
    mask = squircle_mask(T)
    img = Image.new("RGBA", (C, C), (0, 0, 0, 0))
    # drop shadow
    sh = Image.new("L", (C, C), 0)
    sh.paste(mask.point(lambda v: v * 0.55), (OX * SS, OY * SS + 3 * SS))
    sh = sh.filter(ImageFilter.GaussianBlur(3 * SS))
    img.alpha_composite(Image.merge("RGBA", (*(Image.new("L", (C, C), 0),) * 3, sh)))
    # tile: gradient, a soft sheen on the top half
    tile = gradient(T, top, bot).convert("RGBA")
    sheen = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    ImageDraw.Draw(sheen).ellipse([-T * 0.3, -T * 0.75, T * 1.3, T * 0.45], fill=(255, 255, 255, 26))
    tile.alpha_composite(sheen)
    if glyph == "8ball":
        tile.alpha_composite(eightball(T))
    elif glyph == "gotchi":
        tile.alpha_composite(gotchi(T))
    else:
        tile.alpha_composite(glyph_layer(T, glyph, gcol))
    tile.putalpha(mask)
    img.alpha_composite(tile, (OX * SS, OY * SS))
    return img.resize((CAN, CAN), Image.LANCZOS)


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main():
    icons = [render(t, b, gl, gc) for _, t, b, gl, gc in APPS]
    L = ["// Generated by tools/make_home_icons.py - do not edit.",
         "// Home-screen app tiles: RGB565 (straight, not premultiplied) + 8-bit alpha, %dx%d each." % (CAN, CAN),
         "// Glyphs: Material Symbols Rounded (Google, Apache License 2.0).",
         "#pragma once", "", "#define HICON_S %d" % CAN, "#define HICON_TILE %d" % TILE,
         "#define HICON_OX %d" % OX, "#define HICON_OY %d" % OY, ""]
    for (name, *_), im in zip(APPS, icons):
        px = list(im.get_flattened_data()) if hasattr(im, "get_flattened_data") else list(im.getdata())
        L.append("static const uint16_t HICON_%s_PX[] PROGMEM = {" % name)
        vals = [rgb565(*p[:3]) if p[3] else 0 for p in px]
        for i in range(0, len(vals), 16):
            L.append("  " + ", ".join("0x%04X" % v for v in vals[i:i + 16]) + ",")
        L.append("};")
        L.append("static const uint8_t HICON_%s_A[] PROGMEM = {" % name)
        al = [p[3] for p in px]
        for i in range(0, len(al), 24):
            L.append("  " + ", ".join(str(v) for v in al[i:i + 24]) + ",")
        L.append("};")
    L.append("static const uint16_t *const HICON_PX[%d] = { %s };" % (len(APPS), ", ".join("HICON_%s_PX" % a[0] for a in APPS)))
    L.append("static const uint8_t *const HICON_A[%d] = { %s };" % (len(APPS), ", ".join("HICON_%s_A" % a[0] for a in APPS)))
    OUT.write_text("\n".join(L) + "\n")
    print("wrote", OUT, "-", len(APPS), "icons")
    if "--preview" in sys.argv:
        sheet = Image.new("RGBA", (CAN * 5 + 60, CAN * 2 + 30), (40, 44, 56, 255))
        for i, im in enumerate(icons):
            sheet.alpha_composite(im, (10 + (i % 5) * (CAN + 10), 10 + (i // 5) * (CAN + 10)))
        sheet.convert("RGB").save(ROOT / "preview_home_icons.png")


if __name__ == "__main__":
    main()
