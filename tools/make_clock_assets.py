#!/usr/bin/env python3
"""Clock app assets (v3.27) -> Badge/clock_assets.h

    python tools/make_clock_assets.py assets/clock_dial_src.jpg [cx cy r]

1. The DIAL: the owner's steampunk pocket-watch picture. The circle (cx, cy, r) in the source image is
   cut out and scaled to the 466 px panel. Everything outside radius CLK_WIN (the numeral ring and the
   gilded bezel) is kept as it is; the window in the middle is replaced by a new movement plate (dark
   gilt with circular graining, a hairspring), because the movement in the photo cannot turn. The
   result is stored as a JPEG in flash and decoded once into PSRAM when the app opens.
2. The MOVING PARTS, rendered here at 4x and scaled down (anti-aliased), stored as RGB565 sprites with a
   3-level alpha (0 = see-through, 1 = half, 2 = solid) that the badge rotates every frame:
   six meshing brass gears, the balance wheel, and the hour / minute / second hands.
3. Two static BRIDGES (polished steel, ruby jewels, blued screws) that sit on top of the gears.

The gear train is laid out and phased here so that the teeth really mesh: for every pair the child's
starting angle is solved so that a tooth of one sits in a gap of the other, and its speed is
-(n_parent / n_child) times the parent's. Those numbers are written into the header (CLK_GEARS).
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "Badge" / "clock_assets.h"
N = 466
C = N / 2
CLK_WIN = 137          # movement window radius on the 466 px dial (inner edge of the numeral ring)
SS = 4                 # supersampling for all rendered parts
MOD = 2.4              # gear module (px of pitch diameter per tooth)
BAL_R = 46             # balance wheel radius

rng = np.random.default_rng(7)


# ------------------------------------------------------------------ helpers
def lerp(a, b, t):
    return a + (b - a) * t


def to565(rgb):
    r, g, b = (np.clip(rgb, 0, 255).astype(np.uint16)[..., i] for i in range(3))
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def finish(big):
    """RGBA float array at SS x -> PIL RGBA at 1x (box-filtered, so edges are anti-aliased)."""
    img = Image.fromarray(np.clip(big, 0, 255).astype(np.uint8), "RGBA")
    w, h = img.size
    return img.resize((w // SS, h // SS), Image.BOX)


def outline(rgba, dark=(25, 16, 6), px=1):
    """Darken the outer edge of a sprite so it reads against whatever is under it."""
    a = np.asarray(rgba).astype(float)
    alpha = a[..., 3] / 255
    er = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8)).filter(ImageFilter.MinFilter(1 + 2 * px))) / 255
    edge = np.clip(alpha - er, 0, 1)[..., None]
    a[..., :3] = a[..., :3] * (1 - 0.75 * edge) + np.array(dark) * 0.75 * edge
    return Image.fromarray(a.astype(np.uint8), "RGBA")


# ------------------------------------------------------------------ gears
BRASS = np.array([214, 168, 78])
ROSE = np.array([196, 128, 84])
STEEL = np.array([176, 178, 186])
RUBY = np.array([170, 18, 48])


def gear_sprite(teeth, metal=BRASS, spokes=5, dim=1.0):
    r = teeth * MOD / 2
    outer, root = r + MOD, r - 1.25 * MOD
    half = int(math.ceil(outer + 2))
    S = 2 * half * SS
    yy, xx = np.mgrid[0:S, 0:S].astype(float)
    dx, dy = (xx + 0.5) / SS - half, (yy + 0.5) / SS - half
    rad = np.hypot(dx, dy)
    ang = np.arctan2(dy, dx)
    # teeth: inside the tip circle, and within the (tapering) tooth half-width around the nearest tooth
    p = 2 * math.pi / teeth
    k = np.round(ang / p)
    off = np.abs(ang - k * p) * rad                      # arc distance from the tooth centre line (px)
    t = np.clip((rad - root) / (outer - root), 0, 1)
    halfw = lerp(0.30 * p * r, 0.17 * p * r, t)          # base -> tip
    body = (rad <= root) | ((rad <= outer) & (off <= halfw))
    # spokes and cut-outs (bigger wheels only)
    rim_in = root - 2.4 * MOD
    hub = max(5.0, 0.26 * r)
    solid = body.copy()
    if teeth >= 20:
        sw = max(2.6, 0.13 * r)
        sp = 2 * math.pi / spokes
        ks = np.round(ang / sp)
        spoke = np.abs(ang - ks * sp) * rad <= sw / 2
        cut = (rad > hub) & (rad < rim_in) & ~spoke
        solid &= ~cut
    else:
        for i in range(3):                               # small wheels: three round holes
            a = 2 * math.pi * i / 3
            hx, hy = math.cos(a) * r * 0.48, math.sin(a) * r * 0.48
            solid &= np.hypot(dx - hx, dy - hy) > r * 0.2
    # shading (radially symmetric, so it does not "swim" when the wheel turns)
    shade = np.ones_like(rad)
    shade *= np.where(rad > root, 1.08, 1.0)             # teeth a touch brighter
    shade *= np.where((rad > rim_in) & (rad <= root), 1.0 + 0.22 * np.cos((rad - rim_in) / (root - rim_in) * math.pi - math.pi / 2), 1)
    shade *= np.where(rad <= hub, 1.12, 1)
    shade *= 0.92 + 0.08 * np.sin(rad * 2.1)             # turned rings
    shade *= 0.96 + 0.04 * rng.standard_normal(rad.shape).clip(-2, 2)  # grain
    rgb = metal[None, None, :] * (shade * dim)[..., None]
    # bevel lines
    for edge, w, f in ((root, 0.7, 0.62), (rim_in, 0.7, 1.25), (hub, 0.8, 0.6)):
        rgb = np.where((np.abs(rad - edge) < w)[..., None], rgb * f, rgb)
    # steel arbor + ruby jewel in the middle
    arbor = rad < max(3.2, 0.12 * r)
    rgb = np.where(arbor[..., None], STEEL[None, None, :] * (1.05 - 0.2 * rad / 6)[..., None], rgb)
    jewel = rad < max(1.8, 0.07 * r)
    rgb = np.where(jewel[..., None], (RUBY * 1.0)[None, None, :] * (1.4 - 0.5 * rad / 2)[..., None], rgb)
    a = np.where(solid, 255.0, 0.0)
    img = finish(np.dstack([rgb, a]))
    return outline(img), (half, half)


def balance_sprite(R=BAL_R):
    half = R + 6
    S = 2 * half * SS
    yy, xx = np.mgrid[0:S, 0:S].astype(float)
    dx, dy = (xx + 0.5) / SS - half, (yy + 0.5) / SS - half
    rad = np.hypot(dx, dy)
    ang = np.arctan2(dy, dx)
    rim = (rad > R - 4.5) & (rad < R)
    arms = (rad < R) & (np.abs(np.sin(ang * 1.5)) * rad < 1.6) & (np.cos(ang * 1.5) > 0)   # three arms
    hub = rad < 5.5
    screws = np.zeros_like(rad, bool)
    for i in range(14):                                  # timing screws round the rim
        a = 2 * math.pi * i / 14
        screws |= np.hypot(dx - math.cos(a) * (R + 1.5), dy - math.sin(a) * (R + 1.5)) < 2.3
    solid = rim | arms | hub | screws
    rgb = np.ones(rad.shape + (3,)) * BRASS * 1.05
    rgb = np.where(screws[..., None], np.array([150, 112, 48]) * (1.35 - 0.5 * np.clip(rad - R, 0, 3) / 3)[..., None], rgb)
    rgb = np.where(((np.abs(rad - (R - 4.5)) < 0.6) | (np.abs(rad - R) < 0.6))[..., None], rgb * 0.6, rgb)
    rgb = np.where((rad < 2.2)[..., None], RUBY * 1.3, rgb)
    img = finish(np.dstack([rgb, np.where(solid, 255.0, 0)]))
    return outline(img), (half, half)


# ------------------------------------------------------------------ hands (drawn pointing at 12)
GILT = (226, 186, 96)
GILT_DARK = (60, 36, 10)


def hand_sprite(length, tail, width, moon, tip, color=GILT, edge=GILT_DARK, cw=0, cwr=0):
    """Breguet-style hand: tapering shaft, a hollow 'moon' ring, a pointed tip, a short tail."""
    W = int(2 * max(width, moon[1] + 3, cwr + 3, 8)) + 4
    H = length + tail + 6
    px, py = W / 2, tail + 3 + 0.0
    img = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def P(x, y):                                          # hand coords: y up from the pivot
        return ((px + x) * SS, (H - py - y) * SS)

    def shape(grow, fill):
        g = grow
        d.polygon([P(-width / 2 - g, -tail), P(width / 2 + g, -tail), P(width * 0.28 + g, length - tip),
                   P(0, length + g * 1.5), P(-width * 0.28 - g, length - tip)], fill=fill)
        my, mr = moon
        if mr:
            d.ellipse([P(-mr - g, my + mr + g)[0], P(-mr - g, my + mr + g)[1], P(mr + g, my - mr - g)[0], P(mr + g, my - mr - g)[1]], fill=fill)
        d.ellipse([P(-6 - g, 6 + g)[0], P(-6 - g, 6 + g)[1], P(6 + g, -6 - g)[0], P(6 + g, -6 - g)[1]], fill=fill)
        if cwr:
            d.ellipse([P(-cwr - g, -cw + cwr + g)[0], P(-cwr - g, -cw + cwr + g)[1], P(cwr + g, -cw - cwr - g)[0], P(cwr + g, -cw - cwr - g)[1]], fill=fill)

    shape(1.8, edge + (255,))
    shape(0.0, color + (255,))
    my, mr = moon
    if mr:                                                # punch the moon hollow (shows what is under it)
        ri = mr * 0.55
        d.ellipse([P(-ri - 1.2, my + ri + 1.2)[0], P(-ri - 1.2, my + ri + 1.2)[1], P(ri + 1.2, my - ri - 1.2)[0], P(ri + 1.2, my - ri - 1.2)[1]], fill=edge + (255,))
        d.ellipse([P(-ri, my + ri)[0], P(-ri, my + ri)[1], P(ri, my - ri)[0], P(ri, my - ri)[1]], fill=(0, 0, 0, 0))
    if cwr:
        ri = cwr * 0.5
        d.ellipse([P(-ri, -cw + ri)[0], P(-ri, -cw + ri)[1], P(ri, -cw - ri)[0], P(ri, -cw - ri)[1]], fill=(0, 0, 0, 0))
    # a light ridge down the middle of the shaft
    d.line([P(0, 8), P(0, length - tip - 2)], fill=tuple(min(255, int(c * 1.18)) for c in color) + (255,), width=max(1, SS // 2))
    return img.resize((W, H), Image.BOX), (px, H - py)


# ------------------------------------------------------------------ layout: the gear train
# name, teeth, metal, spokes, parent, angle from parent (deg, screen coords) or position for the root
TRAIN = [
    ("A", 40, BRASS, 5, None, (58.8, -49.4)),    # third wheel, upper right - 1 turn per minute
    ("C", 24, ROSE, 5, "A", None),               # centre wheel (under the hands)
    ("H", 18, BRASS, 0, "A", 80),                # right
    ("D", 16, BRASS, 0, "C", 200),               # left
    ("G", 26, BRASS, 6, "D", 240),               # upper left
    ("I", 20, ROSE, 4, "D", 110),                # lower left
    ("T", 22, BRASS, 5, "A", 218),               # top centre
    ("J", 18, BRASS, 0, "I", 120),               # bottom left
]
# Lower-level wheels: each shares an arbor (centre + speed) with a front wheel, like a wheel and its
# pinion in a real movement. Drawn first, darker, partly hidden - they fill the window with brass.
BACK = [("BC", 48, "C"), ("BH", 34, "H"), ("BG", 30, "G"), ("BD", 40, "D")]
BAL = (0.0, 82.0)                                 # balance wheel centre (relative to the dial centre)


def solve_train():
    g = {}
    for name, n, metal, spokes, parent, where in TRAIN:
        r = n * MOD / 2
        if parent is None:
            g[name] = dict(n=n, r=r, x=where[0], y=where[1], phase=0.0, ratio=1.0, metal=metal, spokes=spokes)
            continue
        P = g[parent]
        if name == "C":                              # the centre wheel sits exactly in the middle
            x, y = 0.0, 0.0
            # then A must be exactly r_A + r_C away: put A on that circle in its chosen direction
            d = math.hypot(P["x"], P["y"])
            P["x"], P["y"] = P["x"] / d * (P["r"] + r), P["y"] / d * (P["r"] + r)
        else:
            a = math.radians(where)
            x, y = P["x"] + math.cos(a) * (P["r"] + r), P["y"] + math.sin(a) * (P["r"] + r)
        th = math.atan2(y - P["y"], x - P["x"])          # direction parent -> child
        # mesh: a tooth of the parent points at th, a gap of the child points back at th + pi
        u1 = (th - P["phase"]) * P["n"] / (2 * math.pi)
        phase = th + math.pi - (2 * math.pi / n) * (0.5 - u1)
        g[name] = dict(n=n, r=r, x=x, y=y, phase=phase, ratio=-P["ratio"] * P["n"] / n, metal=metal, spokes=spokes)
    return g


# ------------------------------------------------------------------ the dial
def make_dial(src, cx, cy, r, gears):
    im = Image.open(src).convert("RGB")
    dial = im.crop((cx - r, cy - r, cx + r, cy + r)).resize((N, N), Image.LANCZOS)
    a = np.asarray(dial).astype(float)
    yy, xx = np.mgrid[0:N, 0:N].astype(float)
    dx, dy = xx + 0.5 - C, yy + 0.5 - C
    rad = np.hypot(dx, dy)
    # movement plate: dark gilt with perlage (overlapping circular graining)
    plate = np.ones((N, N, 3)) * np.array([74, 54, 26])
    perl = np.zeros((N, N))
    step = 9
    for gy in range(-CLK_WIN, CLK_WIN + step, step):
        for gx in range(-CLK_WIN, CLK_WIN + step, step):
            ox, oy = gx + (gy // step % 2) * step / 2 + C, gy + C
            d = np.hypot(xx - ox, yy - oy)
            perl = np.maximum(perl, np.clip(1 - d / 7.5, 0, 1) * (0.5 + 0.5 * np.sin(d * 1.4)))
    plate *= (0.74 + 0.36 * perl)[..., None]
    plate *= (1.0 - 0.45 * np.clip((rad - (CLK_WIN - 22)) / 22, 0, 1) ** 1.5)[..., None]   # recessed edge
    # sinks under each wheel (a darker turned recess)
    for gg in gears.values():
        d = np.hypot(dx - gg["x"], dy - gg["y"])
        plate *= np.where(d < gg["r"] + MOD + 3, 0.78, 1.0)[..., None]
    d = np.hypot(dx - BAL[0], dy - BAL[1])
    plate *= np.where(d < BAL_R + 7, 0.72, 1.0)[..., None]
    # hairspring under the balance (static)
    spring = np.zeros((N, N), bool)
    ang = np.arctan2(dy - BAL[1], dx - BAL[0]) % (2 * math.pi)
    for turn in range(7):
        rr = 7 + (ang / (2 * math.pi) + turn) * 4.2
        spring |= np.abs(d - rr) < 0.55
    plate = np.where((spring & (d < 37))[..., None], np.array([200, 205, 215]) * 0.85, plate)
    a = np.where((rad < CLK_WIN)[..., None], plate, a)
    a = np.where((rad > N / 2 - 0.5)[..., None], 0, a)   # outside the round panel: black
    return Image.fromarray(a.clip(0, 255).astype(np.uint8))


def make_bridges(gears):
    """Static steel bridges + jewels + blued screws, as one RGBA layer the size of the window."""
    W = 2 * CLK_WIN
    img = Image.new("RGBA", (W * SS, W * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    P = lambda x, y: ((x + CLK_WIN) * SS, (y + CLK_WIN) * SS)

    def bar(pts, w, fill):
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            d.line([P(x1, y1), P(x2, y2)], fill=fill, width=int(w * SS))
        for x, y in pts:
            d.ellipse([P(x - w / 2, y - w / 2), P(x + w / 2, y + w / 2)], fill=fill)

    G, D_, I = gears["G"], gears["D"], gears["I"]
    train = [(G["x"], G["y"]), (D_["x"], D_["y"]), (I["x"], I["y"])]
    cock = [(math.cos(math.radians(22)) * 140, math.sin(math.radians(22)) * 140 + 14), (BAL[0] + 26, BAL[1] - 4), (BAL[0], BAL[1])]
    for pts, w in ((train, 21), (cock, 24)):
        bar(pts, w, (255, 255, 255, 255))                 # shape only - shaded below
    jewels = train + [cock[1]]
    screws = [((train[0][0] + train[1][0]) / 2, (train[0][1] + train[1][1]) / 2),
              ((train[1][0] + train[2][0]) / 2, (train[1][1] + train[2][1]) / 2),
              ((cock[0][0] * 0.62 + cock[1][0] * 0.38), (cock[0][1] * 0.62 + cock[1][1] * 0.38))]
    out = img.resize((W, W), Image.BOX)
    m = np.asarray(out).astype(float)
    # Shade the bridge shapes: distance from the edge (by repeated erosion) gives a polished chamfer
    # and a dark lip; the flat top gets diagonal Geneva stripes. Then jewels and screws on top.
    solid = (m[..., 3] > 127).astype(np.uint8) * 255
    dist = np.zeros(solid.shape)
    cur = Image.fromarray(solid)
    for k in range(1, 6):
        cur = cur.filter(ImageFilter.MinFilter(3))
        dist += (np.asarray(cur) > 127)
    yy, xx = np.mgrid[0:W, 0:W].astype(float)
    stripes = 0.5 + 0.5 * np.sin((xx * 0.7 + yy * 0.7) * 2 * math.pi / 10)
    base = np.array([150, 153, 164])[None, None, :] * (0.86 + 0.2 * stripes)[..., None]
    lit = 0.5 + 0.5 * np.clip((W - xx - yy) / W, -1, 1)     # light from the upper left
    rgb = np.where((dist == 0)[..., None], np.array([40, 40, 48]),
          np.where((dist == 1)[..., None], np.array([222, 224, 232]) * (0.75 + 0.35 * lit)[..., None],
          np.where((dist == 2)[..., None], np.array([120, 122, 132]) * (0.8 + 0.3 * lit)[..., None], base)))
    m[..., :3] = np.where((m[..., 3] > 0)[..., None], rgb, m[..., :3])
    deco = Image.new("RGBA", (W * SS, W * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(deco)
    for x, y in jewels:
        for rr, col in ((6.6, (70, 50, 25)), (5.8, (214, 180, 96)), (3.6, (120, 10, 30)), (2.4, (205, 40, 72)), (1.0, (255, 180, 200))):
            d.ellipse([P(x - rr, y - rr), P(x + rr, y + rr)], fill=col + (255,))
    for (x, y) in screws:
        d.ellipse([P(x - 4.2, y - 4.2), P(x + 4.2, y + 4.2)], fill=(20, 20, 30, 255))
        d.ellipse([P(x - 3.4, y - 3.4), P(x + 3.4, y + 3.4)], fill=(46, 76, 168, 255))   # blued screw
        d.line([P(x - 2.8, y - 1.3), P(x + 2.8, y + 1.3)], fill=(10, 15, 40, 255), width=SS)
    dd = np.asarray(deco.resize((W, W), Image.BOX)).astype(float)
    da = dd[..., 3:4] / 255
    m[..., :3] = m[..., :3] * (1 - da) + dd[..., :3] * da
    m[..., 3] = np.maximum(m[..., 3], dd[..., 3])
    m = m.clip(0, 255).astype(np.uint8)
    yy, xx = np.mgrid[0:W, 0:W]
    m[np.hypot(xx + 0.5 - CLK_WIN, yy + 0.5 - CLK_WIN) > CLK_WIN - 1, 3] = 0           # stay inside the window
    ys, xs = np.nonzero(m[..., 3] >= 64)                                                # crop to what is drawn
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    return Image.fromarray(m[y0:y1, x0:x1], "RGBA"), (CLK_WIN - x0, CLK_WIN - y0)


# ------------------------------------------------------------------ output
def sprite_c(name, img, pivot, lines, spans=False):
    a = np.asarray(img).astype(float)
    alpha = a[..., 3]
    q = np.where(alpha >= 192, 2, np.where(alpha >= 64, 1, 0)).astype(np.uint8)
    ys, xs = np.nonzero(q)
    reach = float(np.max(np.hypot(xs + 1 - pivot[0], ys + 1 - pivot[1]))) + 1 if len(xs) else 0
    pix = to565(a[..., :3]).flatten()
    w, h = img.size
    lines.append(f"static const uint16_t {name}_px[{w * h}] PROGMEM = {{")
    for i in range(0, len(pix), 20):
        lines.append("  " + ",".join(f"0x{v:04X}" for v in pix[i:i + 20]) + ",")
    lines.append("};")
    qa = q.flatten()
    packed = [(qa[i] | (qa[i + 1] << 2 if i + 1 < len(qa) else 0) | (qa[i + 2] << 4 if i + 2 < len(qa) else 0) |
               (qa[i + 3] << 6 if i + 3 < len(qa) else 0)) for i in range(0, len(qa), 4)]
    lines.append(f"static const uint8_t {name}_a[{len(packed)}] PROGMEM = {{")
    for i in range(0, len(packed), 32):
        lines.append("  " + ",".join(str(v) for v in packed[i:i + 32]) + ",")
    lines.append("};")
    lines.append(f"static const ClkSprite {name} = {{ {w}, {h}, {pivot[0]:.1f}f, {pivot[1]:.1f}f, {reach:.1f}f, {name}_px, {name}_a }};")
    if spans:                                     # static layer: first / last visible pixel of each row
        L_, R_ = [], []
        for y in range(h):
            nz = np.nonzero(q[y])[0]
            L_.append(int(nz[0]) if len(nz) else 1); R_.append(int(nz[-1]) if len(nz) else 0)
        lines.append(f"static const uint16_t {name}_l[{h}] PROGMEM = {{ " + ",".join(map(str, L_)) + " };")
        lines.append(f"static const uint16_t {name}_r[{h}] PROGMEM = {{ " + ",".join(map(str, R_)) + " };")
    lines.append("")
    return w * h


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "assets" / "clock_dial_src.jpg")
    cx, cy, r = (float(v) for v in sys.argv[2:5]) if len(sys.argv) >= 5 else (209.5, 229.0, 197.5)
    gears = solve_train()
    dial = make_dial(src, cx, cy, r, gears)
    jpg = ROOT / "Badge" / "_clock_dial.jpg"
    dial.save(jpg, quality=90, subsampling=0)
    jb = jpg.read_bytes()
    jpg.unlink()
    dial.save(Path(__file__).resolve().parent / "_clock_dial_preview.png") if "--keep" in sys.argv else None

    L = ["// Generated by tools/make_clock_assets.py - do not edit by hand.", "#pragma once", "#include <stdint.h>", "",
         "// w x h RGB565 pixels, pivot (px, py), reach = furthest visible pixel from the pivot,",
         "// a = 2-bit alpha, 4 per byte (0 see-through, 1 half, 2 solid)",
         "typedef struct { uint16_t w, h; float px, py, reach; const uint16_t *pix; const uint8_t *a; } ClkSprite;", "",
         f"#define CLK_WIN {CLK_WIN}", f"#define CLK_BAL_X {BAL[0]:.1f}f", f"#define CLK_BAL_Y {BAL[1]:.1f}f", "",
         f"static const uint8_t CLK_DIAL_JPG[{len(jb)}] PROGMEM = {{"]
    for i in range(0, len(jb), 32):
        L.append("  " + ",".join(str(b) for b in jb[i:i + 32]) + ",")
    L.append("};")
    L.append("")
    total = 0
    order = []
    for name, teeth, front in BACK:                     # lower level first: they are drawn first
        f = gears[front]
        gears[name] = dict(n=teeth, r=teeth * MOD / 2, x=f["x"], y=f["y"], phase=f["phase"] + 0.3, ratio=f["ratio"], metal=BRASS, spokes=6)
        img, piv = gear_sprite(teeth, BRASS, 6, dim=0.72)
        total += sprite_c(f"CLK_GEAR_{name}", img, piv, L)
        order.append(name)
    for name, *_ in TRAIN:
        gg = gears[name]
        img, piv = gear_sprite(gg["n"], gg["metal"], gg["spokes"] or 5)
        total += sprite_c(f"CLK_GEAR_{name}", img, piv, L)
        order.append(name)
    img, piv = balance_sprite()
    total += sprite_c("CLK_BALANCE", img, piv, L)
    img, piv = make_bridges(gears)
    total += sprite_c("CLK_BRIDGES", img, piv, L, spans=True)
    hands = {
        "CLK_HAND_H": hand_sprite(112, 20, 10.0, (76, 12), 18),
        "CLK_HAND_M": hand_sprite(184, 24, 8.0, (134, 9), 20),
        "CLK_HAND_S": hand_sprite(196, 44, 2.6, (0, 0), 10, color=(200, 34, 44), edge=(50, 8, 10), cw=30, cwr=7),
    }
    for name, (img, piv) in hands.items():
        total += sprite_c(name, img, piv, L)
    L.append("// The train: x, y (from the dial centre), start angle, turns per turn of wheel A")
    L.append("typedef struct { const ClkSprite *s; float x, y, phase, ratio; } ClkGear;")
    L.append("static const ClkGear CLK_GEARS[] = {")
    for name in order:
        gg = gears[name]
        L.append(f"  {{ &CLK_GEAR_{name}, {gg['x']:.2f}f, {gg['y']:.2f}f, {gg['phase']:.5f}f, {gg['ratio']:.6f}f }},   // {name}: {gg['n']} teeth")
    L.append("};")
    L.append(f"#define CLK_NGEARS {len(order)}")
    OUT.write_text("\n".join(L) + "\n", encoding="ascii")
    print(f"wrote {OUT}: dial jpeg {len(jb)} bytes, {total} sprite pixels (~{total * 2.25 / 1024:.0f} kB)")
    for name in order:
        gg = gears[name]
        far = math.hypot(gg["x"], gg["y"]) + gg["r"] + MOD
        print(f"  gear {name}: {gg['n']:2d} teeth at ({gg['x']:6.1f},{gg['y']:6.1f}) reaches r={far:5.1f}  ratio {gg['ratio']:+.3f}")


if __name__ == "__main__":
    main()
