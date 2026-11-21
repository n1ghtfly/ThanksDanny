#!/usr/bin/env python3
"""Clock app assets, CYBERPUNK face (v3.29) -> Badge/clock_assets.h

    python tools/make_clock_assets.py            # writes the header
    python tools/make_clock_assets.py --preview  # also writes preview_clock.png (a simulated frame)

The face is a green/amber radar HUD. What never changes is drawn here into one background JPEG
(rings, ticks, decorative arcs, the polar grid, the minute numbers). What changes is drawn by the
badge every frame (Badge/Clock.ino):
  - the radar SWEEP: green light added to the background, by a per-pixel angle table;
  - the outer TEXT RING (a pre-rendered text strip, mapped onto the ring; static since v3.30);
  - the SECONDS RING: 120 amber bars that fill up through the minute;
  - the TIME in big glowing amber digits (pre-rendered with their glow, added like light);
  - the HUD text: status, badge name, Wi-Fi, uptime, date.

Fonts (SIL Open Font License, see assets/fonts/): Black Ops One (the digits), Share Tech Mono (HUD).
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "Badge" / "clock_assets.h"
FONTS = ROOT / "assets" / "fonts"
DIGIT_TTF = str(FONTS / "BlackOpsOne-Regular.ttf")
HUD_TTF = str(FONTS / "ShareTechMono-Regular.ttf")
N, C = 466, 233.0
SS = 4

# ---- layout (radii in px on the 466 px panel)
R_LINE1, R_LINE2 = 230.5, 226.5          # outer double orange line
TXT_R1, TXT_R0 = 223, 207                # scrolling text band (outer, inner)
R_LINE3 = 204.0
TICK_R1, TICK_R0 = 201, 190              # green tick ring
R_LINE4 = 186.5
BAR_R1, BAR_R0 = 183, 167                # seconds bars (drawn by the badge)
R_LINE5 = 163.5
R_INNER = 156.0                          # bright green circle; the sweep stays inside this
SWEEP_R = 154
NUM_R = 140                              # minute numbers

GREEN = np.array([60, 255, 110])
AMBER = np.array([255, 170, 30])
ORANGE = np.array([255, 120, 20])

MARQUEE = [("SONS OF SUDO", GREEN), ("  *  ", GREEN), ("SYS.LEVEL: ROOT", GREEN), ("  *  ", GREEN),
           ("BGP PEERING: STABLE", GREEN), ("  *  ", GREEN),
           ("THE UNEXAMINED NETWORK IS NOT WORTH CRASHING. - SOCRATES", ORANGE), ("  *  ", GREEN),
           ("IT IS ALWAYS DNS", GREEN), ("  *  ", GREEN)]


# ---------------------------------------------------------------- helpers
def to565(rgb):
    r, g, b = (np.clip(rgb, 0, 255).astype(np.uint16)[..., i] for i in range(3))
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def polar():
    yy, xx = np.mgrid[0:N, 0:N].astype(float)
    dx, dy = xx + 0.5 - C, yy + 0.5 - C
    return np.hypot(dx, dy), (np.arctan2(dx, -dy)) % (2 * math.pi)   # angle clockwise from 12


def ring(img, r, w, col, glow=0.0):
    """Anti-aliased ring of width w at radius r, added as light (with an optional soft glow)."""
    rad, _ = polar()
    a = np.clip(w / 2 + 0.5 - np.abs(rad - r), 0, 1)
    img += a[..., None] * col
    if glow:
        g = np.exp(-((rad - r) / 3.0) ** 2) * glow
        img += g[..., None] * col


# ---------------------------------------------------------------- GFX font (Adafruit format)
def gfx_font(ttf, px, name, out):
    font = ImageFont.truetype(ttf, px)
    data, glyphs = bytearray(), []
    for code in range(0x20, 0x7F):
        ch = chr(code)
        mask, (ox, oy) = font.getmask2(ch, mode="1", anchor="ls")
        w, h = mask.size
        adv = int(round(font.getlength(ch)))
        off = len(data)
        acc, n = 0, 0
        for y in range(h):
            for x in range(w):
                acc = (acc << 1) | (1 if mask.getpixel((x, y)) else 0)
                n += 1
                if n == 8:
                    data.append(acc); acc, n = 0, 0
        if n:
            data.append(acc << (8 - n))
        glyphs.append((off, w, h, adv, ox, oy))
    asc, desc = font.getmetrics()
    cap = -font.getbbox("H", anchor="ls")[1]
    out.append(f"// {name}: {Path(ttf).stem} {px} px, caps {cap} px")
    out.append(f"static const uint8_t {name}Bitmaps[] PROGMEM = {{")
    for i in range(0, len(data), 24):
        out.append("  " + ",".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",")
    out.append("};")
    out.append(f"static const GFXglyph {name}Glyphs[] PROGMEM = {{")
    for (off, w, h, adv, ox, oy) in glyphs:
        out.append(f"  {{ {off}, {w}, {h}, {adv}, {ox}, {oy} }},")
    out.append("};")
    out.append(f"static const GFXfont {name} PROGMEM = {{ (uint8_t *){name}Bitmaps, (GFXglyph *){name}Glyphs, 0x20, 0x7E, {asc + desc} }};")
    out.append(f"#define {name.upper()}_CAP {cap}")
    out.append("")
    return font


# ---------------------------------------------------------------- the static background
def make_background():
    img = np.zeros((N, N, 3))
    rad, ang = polar()
    deg = np.degrees(ang)
    # faint scanlines + a dark green haze in the middle
    img += (np.exp(-(rad / 190) ** 2) * 14)[..., None] * np.array([0.2, 1.0, 0.45])
    # rings
    ring(img, R_LINE1, 1.6, ORANGE * 0.9, glow=0.25)
    ring(img, R_LINE2, 1.0, ORANGE * 0.6)
    ring(img, R_LINE3, 1.4, ORANGE * 0.85, glow=0.2)
    ring(img, R_LINE4, 1.0, ORANGE * 0.55)
    ring(img, R_LINE5, 1.4, ORANGE * 0.8, glow=0.2)
    ring(img, R_INNER, 2.2, GREEN * 0.95, glow=0.45)
    # tick ring: a fine tick every 2 degrees, a long one every 10, decorative arcs
    tk = np.abs(((deg + 1) % 2) - 1) * rad * math.pi / 180      # px from nearest 2-degree tick
    long_ = np.abs(((deg + 5) % 10) - 5) * rad * math.pi / 180
    fine = (tk < 0.6) & (rad > TICK_R1 - 4) & (rad < TICK_R1)
    lng = (long_ < 0.8) & (rad > TICK_R0) & (rad < TICK_R1)
    img += fine[..., None] * GREEN * 0.45 + lng[..., None] * GREEN * 0.85
    arc = (rad > TICK_R0 + 1) & (rad < TICK_R0 + 6)
    img += (arc & ((deg > 290) | (deg < 40)))[..., None] * GREEN * 0.55          # top
    img += (arc & (deg > 120) & (deg < 150))[..., None] * ORANGE * 0.5            # lower right
    img += (arc & (deg > 215) & (deg < 245))[..., None] * GREEN * 0.4             # lower left
    # inner polar grid (faint)
    for r in (52, 104):
        ring(img, r, 1.0, GREEN * 0.16)
    spoke = (np.abs(((deg + 15) % 30) - 15) * rad * math.pi / 180 < 0.6) & (rad < R_INNER - 2) & (rad > 8)
    img += spoke[..., None] * GREEN * 0.14
    # minute ticks just inside the green circle
    mt = (np.abs(((deg + 3) % 6) - 3) * rad * math.pi / 180 < 0.7) & (rad > R_INNER - 7) & (rad < R_INNER - 2)
    img += mt[..., None] * GREEN * 0.6
    # scanlines
    img *= (0.92 + 0.08 * (np.arange(N) % 3 != 0))[:, None, None]
    pil = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    # minute numbers 60, 5, 10 ... 55
    d = ImageDraw.Draw(pil)
    f = ImageFont.truetype(HUD_TTF, 13)
    for i in range(12):
        a = math.radians(i * 30)
        x, y = C + math.sin(a) * NUM_R, C - math.cos(a) * NUM_R
        d.text((x, y), str(60 if i == 0 else i * 5), font=f, fill=(60, 230, 110), anchor="mm")
    a = np.asarray(pil).astype(float)
    a[rad > N / 2 - 0.5] = 0
    return Image.fromarray(a.astype(np.uint8))


# ---------------------------------------------------------------- glowing digits (added like light)
def digit_sprites(px=50):
    f = ImageFont.truetype(DIGIT_TTF, px)
    chars = "0123456789:"
    adv = max(f.getlength(c) for c in "0123456789")
    pad = 8
    asc = -f.getbbox("0", anchor="ls")[1]
    H = asc + 2 * pad
    out = {}
    for ch in chars:
        w = int(math.ceil(f.getlength(ch))) + 2 * pad
        m = Image.new("L", (w * SS, H * SS), 0)
        ImageDraw.Draw(m).text((pad * SS, (pad + asc) * SS), ch, font=ImageFont.truetype(DIGIT_TTF, px * SS), fill=255, anchor="ls")
        m = m.resize((w, H), Image.BOX)
        core = np.asarray(m).astype(float) / 255
        glow = np.asarray(m.filter(ImageFilter.GaussianBlur(5))).astype(float) / 255
        # vertical gradient on the face: brighter at the top, like the reference
        grad = np.linspace(1.08, 0.82, H)[:, None]
        rgb = core[..., None] * np.array([255, 196, 52]) * grad[..., None] + (glow * (1 - core))[..., None] * np.array([255, 110, 0]) * 0.75
        out[ch] = rgb
    return out, int(round(adv)), int(round(f.getlength(":"))), H, pad, asc


# ---------------------------------------------------------------- the scrolling text strip
def marquee_strip():
    f = ImageFont.truetype(HUD_TTF, 17)
    H = TXT_R1 - TXT_R0
    circ = 2 * math.pi * (TXT_R0 + TXT_R1) / 2
    parts = list(MARQUEE)
    width = sum(f.getlength(t) for t, _ in parts)
    while width < circ * 0.92:                      # long enough to go once round
        parts += MARQUEE
        width = sum(f.getlength(t) for t, _ in parts)
    L = int(round(width))
    img = Image.new("RGB", (L * SS, H * SS), (0, 0, 0))
    d = ImageDraw.Draw(img)
    fb = ImageFont.truetype(HUD_TTF, 17 * SS)
    x = 0.0
    for t, col in parts:
        d.text((x * SS, (H - 3) * SS), t, font=fb, fill=tuple(int(c) for c in col), anchor="ls")
        x += f.getlength(t)
    strip = np.asarray(img.resize((L, H), Image.BOX)).astype(float)
    return strip, L, H


# ---------------------------------------------------------------- output
def arr_c(name, vals, typ, per=20, fmt="{}"):
    lines = [f"static const {typ} {name}[{len(vals)}] PROGMEM = {{"]
    for i in range(0, len(vals), per):
        lines.append("  " + ",".join(fmt.format(v) for v in vals[i:i + per]) + ",")
    lines.append("};")
    return lines


def main():
    bg = make_background()
    tmp = ROOT / "Badge" / "_bg.jpg"
    bg.save(tmp, quality=92, subsampling=0)
    jb = tmp.read_bytes()
    tmp.unlink()
    L = ["// Generated by tools/make_clock_assets.py - do not edit by hand.", "#pragma once", "#include <stdint.h>", "",
         f"#define CLK_TXT_R1 {TXT_R1}", f"#define CLK_TXT_R0 {TXT_R0}", f"#define CLK_BAR_R1 {BAR_R1}",
         f"#define CLK_BAR_R0 {BAR_R0}", f"#define CLK_SWEEP_R {SWEEP_R}", ""]
    L += arr_c("CLK_BG_JPG", list(jb), "uint8_t", 40)
    L.append("")
    gfx_font(HUD_TTF, 13, "HudSmall", L)
    gfx_font(HUD_TTF, 17, "HudMid", L)
    dig, adv, cadv, H, pad, asc = digit_sprites()
    L.append(f"// Time digits: additive RGB565 (light to add), {H} px high incl. {pad} px of glow each side")
    L.append(f"#define CLK_DIG_H {H}\n#define CLK_DIG_PAD {pad}\n#define CLK_DIG_ADV {adv}\n#define CLK_COLON_ADV {cadv}")
    order = "0123456789:"
    for ch in order:
        nm = "CLK_D_" + ("COLON" if ch == ":" else ch)
        L += arr_c(nm, [f"0x{v:04X}" for v in to565(dig[ch]).flatten()], "uint16_t", 24)
    L.append("typedef struct { uint16_t w; const uint16_t *px; } ClkDigit;")
    L.append("static const ClkDigit CLK_DIGITS[11] = { " + ", ".join(
        f"{{ {dig[ch].shape[1]}, CLK_D_{'COLON' if ch == ':' else ch} }}" for ch in order) + " };")
    L.append("")
    strip, SL, SH = marquee_strip()
    L.append(f"// Scrolling text ring: {SH} rows (row 0 = outer edge) x {SL} columns, RGB565")
    L.append(f"#define CLK_STRIP_W {SL}\n#define CLK_STRIP_H {SH}")
    L += arr_c("CLK_STRIP", [f"0x{v:04X}" for v in to565(strip).flatten()], "uint16_t", 24)
    OUT.write_text("\n".join(L) + "\n", encoding="ascii")
    print(f"wrote {OUT}: bg jpeg {len(jb)} B, digits {adv} px wide x {H}, strip {SL} x {SH}")
    if "--preview" in sys.argv:
        preview(bg, dig, adv, cadv, H, pad, strip, SL, SH).save(ROOT / "preview_clock.png")
        print("wrote preview_clock.png")


# The HUD text as Clock.ino lays it out (baseline y); used only for the preview.
HUD_PREVIEW = [(C, 140, "[ SYSTEM STATUS: SECURE ]", "s", (150, 255, 170), "ms"),
               (C, 162, "SONS OF SUDO // DANNY", "m", (70, 255, 120), "ms"),
               (C - 10, 196, "WIFI -63dBm", "s", (60, 220, 110), "rs"),
               (C + 10, 196, "UP 147h32m", "s", (60, 220, 110), "ls"),
               (C, 286, "STARDATE 2026.11.21 SAT", "m", (70, 255, 120), "ms"),
               (C, 306, "CET", "s", (255, 170, 40), "ms")]
TIME_Y = C + 4                                   # middle of the digits


# ---------------------------------------------------------------- a simulated frame (same maths as Clock.ino)
def preview(bg, dig, adv, cadv, H, pad, strip, SL, SH, t=(18, 22, 15.3), scroll=120.0, beam=45.0):
    fb = np.asarray(bg).astype(float)
    rad, ang = polar()
    # sweep: green added behind the beam, 90 degrees of trail
    d = (math.radians(beam) - ang) % (2 * math.pi)
    trail = math.radians(90)
    k = np.where((d < trail) & (rad < SWEEP_R), (1 - d / trail) ** 2, 0)
    fb += k[..., None] * np.array([20, 120, 45])
    fb += ((d < 0.025) & (rad < SWEEP_R))[..., None] * np.array([90, 255, 140])
    # text ring
    band = (rad >= TXT_R0) & (rad < TXT_R1)
    col = ((ang / (2 * math.pi) * SL + scroll) % SL).astype(int)
    row = np.clip((TXT_R1 - 1 - rad).astype(int), 0, SH - 1)
    fb[band] = strip[row[band], col[band]]
    # seconds bars
    img = Image.fromarray(np.clip(fb, 0, 255).astype(np.uint8))
    dr = ImageDraw.Draw(img)
    s = t[2]
    for i in range(120):
        a0, a1 = math.radians(i * 3 - 0.8), math.radians(i * 3 + 0.8)
        lit = i <= s * 2
        c = (255, 205, 80) if lit and i > s * 2 - 4 else (255, 140, 20) if lit else (70, 30, 4)
        pts = [(C + math.sin(a) * r, C - math.cos(a) * r) for a, r in ((a0, BAR_R0), (a0, BAR_R1), (a1, BAR_R1), (a1, BAR_R0))]
        dr.polygon(pts, fill=c)
    fb = np.asarray(img).astype(float)
    # time
    txt = f"{t[0]:02d}:{t[1]:02d}:{int(t[2]):02d}"
    total = sum(cadv if ch == ":" else adv for ch in txt)
    x = C - total / 2
    y0 = int(TIME_Y - H / 2)
    for ch in txt:
        sp = dig[ch]
        slot = cadv if ch == ":" else adv
        sx = int(x + (slot - (sp.shape[1] - 2 * pad)) / 2 - pad)
        fb[y0:y0 + sp.shape[0], sx:sx + sp.shape[1]] += sp
        x += slot
    img = Image.fromarray(np.clip(fb, 0, 255).astype(np.uint8))
    dr = ImageDraw.Draw(img)
    fs, fm = ImageFont.truetype(HUD_TTF, 13), ImageFont.truetype(HUD_TTF, 17)
    for (x, y, t, f_, col, an) in HUD_PREVIEW:
        dr.text((x, y), t, font=fs if f_ == "s" else fm, fill=col, anchor=an)
    a = np.asarray(img).copy()
    a[rad > N / 2 - 0.5] = 0
    return Image.fromarray(a)


if __name__ == "__main__":
    main()
