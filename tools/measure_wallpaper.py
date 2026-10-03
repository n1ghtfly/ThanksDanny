"""Find where the Sons of Sudo sign sits in the wallpaper the badge actually draws.

The home screen holds this image full-bleed, so its bright sign is the thing the Pictures
button must sit UNDER rather than overlap. Measuring beats eyeballing a photo taken at an angle.
"""
from PIL import Image, ImageStat

p = r"C:\Users\brain\Documents\Arduino\ThanksDanny\data\wallpaper\son_of_sudo.jpg"
im = Image.open(p).convert("RGB")
print(f"{p.split(chr(92))[-1]}: {im.width}x{im.height}")

W, H = im.size
# The sign is centred, so look at the middle band of columns only.
x0, x1 = int(W * 0.28), int(W * 0.72)
band = im.crop((x0, 0, x1, H))
rows = []
for y in range(H):
    row = band.crop((0, y, band.width, y + 1))
    rows.append(ImageStat.Stat(row).mean[0])   # mean red channel = brightness proxy

peak = max(rows)
print(f"central band x={x0}..{x1}, brightest row {rows.index(peak)} (mean {peak:.0f})")
print()
print("brightness profile (mean red of the central band, per 10-row block):")
for y in range(0, H, 10):
    chunk = rows[y:y + 10]
    m = sum(chunk) / len(chunk)
    bar = "#" * int(m / 4)
    print(f"  y{y:3d}-{y+9:3d} {m:6.1f} {bar}")

# The sign proper: contiguous rows brighter than half the peak.
half = peak / 2
ys = [y for y in range(H) if rows[y] > half]
if ys:
    # longest contiguous run
    best = cur = [ys[0]]
    for y in ys[1:]:
        cur = cur + [y] if y == cur[-1] + 1 else [y]
        if len(cur) > len(best):
            best = cur
    print()
    print(f"sign (rows brighter than {half:.0f}): y={best[0]}..{best[-1]}")
