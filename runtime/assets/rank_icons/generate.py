#!/usr/bin/env python3
"""MKW100 position icons 13th-255th at the game's own size (180x128, RGB5A3 TPL).

The game's 1st-12th (Race_E.szs, tt_position_no_st_64x64_NN.tpl) are 180x128: digits fill the
height (ink y 5..124), the suffix sits small at the lower right. Earlier MKW100 icons were 64x64
and the HUD stretched them into the same pane, so they looked short and soft. This renders each
place once at 4x and downsamples once (premultiplied), with the digits kept at full height:
wide numbers are narrowed, never shortened.
"""
from __future__ import annotations
import argparse, struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops

W, H, S = 180, 128, 4
ROOT = Path(__file__).resolve().parent
FONT = ROOT / 'DejaVuSansCondensed-BoldOblique.ttf'

PALETTES = {
    # top, middle, bottom of the fill; outline; rim highlight
    'blue': ((229, 255, 252), (69, 218, 226), (44, 115, 204), (6, 14, 38), (210, 246, 255)),
    'green': ((236, 255, 214), (112, 232, 92), (36, 150, 52), (8, 30, 10), (220, 255, 200)),
}

def ordinal(n: int) -> str:
    if 11 <= n % 100 <= 13: return 'th'
    return {1: 'st', 2: 'nd', 3: 'rd'}.get(n % 10, 'th')

def text_mask(text: str, height: int) -> Image.Image:
    """Ink mask of `text` whose ink height is `height` pixels (at 4x)."""
    size = int(height * 1.4)
    for _ in range(8):
        font = ImageFont.truetype(str(FONT), size)
        b = font.getbbox(text, anchor='ls')
        ink = b[3] - b[1]
        size = max(8, round(size * height / ink))
        if abs(ink - height) <= 1: break
    font = ImageFont.truetype(str(FONT), size)
    b = font.getbbox(text, anchor='ls')
    m = Image.new('L', (b[2] - b[0] + 40, b[3] - b[1] + 40), 0)
    ImageDraw.Draw(m).text((20 - b[0], 20 - b[1]), text, font=font, anchor='ls', fill=255)
    return m.crop(m.getbbox())

def grow(mask: Image.Image, px: float) -> Image.Image:
    r = max(1, round(px * S))
    return mask.filter(ImageFilter.MaxFilter(2 * r + 1))

def shift(mask: Image.Image, dx: int, dy: int) -> Image.Image:
    out = Image.new('L', mask.size, 0); out.paste(mask, (dx, dy)); return out

def gradient(mask: Image.Image, top, mid, bottom, y0: int, y1: int) -> Image.Image:
    img = Image.new('RGBA', mask.size)
    for y in range(mask.size[1]):
        t = min(1.0, max(0.0, (y - y0) / max(1, y1 - y0)))
        a, b, u = (top, mid, t / 0.5) if t < 0.5 else (mid, bottom, (t - 0.5) / 0.5)
        img.paste(tuple(round(a[k] * (1 - u) + b[k] * u) for k in range(3)) + (255,), (0, y, mask.size[0], y + 1))
    img.putalpha(mask)
    return img

def solid(color, mask: Image.Image) -> Image.Image:
    img = Image.new('RGBA', mask.size, color + (0,)); img.putalpha(mask); return img

def render(n: int, palette: str) -> Image.Image:
    top, mid, bottom, edge, rim = PALETTES[palette]
    cw, ch = W * S, H * S
    # Layout in 1x pixels, as the game's 12th: digits ink y 9..114, suffix ink height 40 on the same
    # baseline, all within x 8..170 after the outline and the 3D edge.
    digit_h, suffix_h, baseline = 104 * S, 40 * S, 113 * S
    num = text_mask(str(n), digit_h)
    suf = text_mask(ordinal(n), suffix_h)
    gap = 2 * S
    avail = 158 * S
    width = num.width + gap + suf.width
    # Fit the width the game's digits take: wide numbers are narrowed (never shortened), narrow
    # ones widened up to 1.3x, as chunky as the stock 10th-12th.
    if width > avail or width < avail * 0.92:
        k = min(1.3, (avail * (0.96 if width < avail else 1.0) - gap) / (num.width + suf.width))
        num = num.resize((max(1, round(num.width * k)), num.height), Image.Resampling.BICUBIC)
        suf = suf.resize((max(1, round(suf.width * k)), suf.height), Image.Resampling.BICUBIC)
        width = num.width + gap + suf.width
    x0 = (cw - width) // 2 - 3 * S
    mnum = Image.new('L', (cw, ch), 0); mnum.paste(num, (x0, baseline - num.height))
    msuf = Image.new('L', (cw, ch), 0); msuf.paste(suf, (x0 + num.width + gap, baseline - suf.height))
    full = ImageChops.lighter(mnum, msuf)
    outline = grow(full, 3.2)
    extrude = ImageChops.lighter(shift(outline, 2 * S, 3 * S), shift(outline, 4 * S, 6 * S))
    extrude = ImageChops.lighter(extrude, shift(outline, 3 * S, 4 * S)).filter(ImageFilter.GaussianBlur(0.4 * S))
    rimmask = grow(full, 1.0)
    canvas = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
    canvas.alpha_composite(solid((0, 0, 0), extrude.point(lambda v: round(v * 0.85))))
    canvas.alpha_composite(solid(edge, outline))
    canvas.alpha_composite(solid(rim, rimmask))
    ytop = baseline - digit_h
    canvas.alpha_composite(gradient(mnum.filter(ImageFilter.GaussianBlur(0.25 * S)), top, mid, bottom, ytop, baseline))
    canvas.alpha_composite(gradient(msuf.filter(ImageFilter.GaussianBlur(0.25 * S)), top, mid, bottom, baseline - suffix_h, baseline))
    return canvas.convert('RGBa').resize((W, H), Image.Resampling.LANCZOS).convert('RGBA')

def rgb5a3(r, g, b, a) -> int:
    if a >= 0xE0:
        return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
    return ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)

def tpl_rgb5a3(img: Image.Image) -> bytes:
    """One-image TPL, format 5 (RGB5A3), 4x4 blocks, the same header layout as the game's."""
    w, h = img.size
    px = img.load()
    data = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            for y in range(by, by + 4):
                for x in range(bx, bx + 4):
                    c = px[x, y] if x < w and y < h else (0, 0, 0, 0)
                    data += struct.pack('>H', rgb5a3(*c))
    header = struct.pack('>III', 0x0020AF30, 1, 0x0C)
    table = struct.pack('>II', 0x14, 0)
    image = struct.pack('>HHIIIIIIfBBBB', h, w, 5, 0x40, 0, 0, 1, 1, 0.0, 0, 0, 0, 0)
    head = header + table + image
    head += b'\0' * (0x40 - len(head))
    return bytes(head + data)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, default=ROOT / 'Out180')
    ap.add_argument('--first', type=int, default=13)
    ap.add_argument('--last', type=int, default=255)
    ap.add_argument('--green-until', type=int, default=24, help='places up to this use the green palette')
    ap.add_argument('--only', type=int, nargs='*')
    a = ap.parse_args()
    (a.out / 'PNG').mkdir(parents=True, exist_ok=True); (a.out / 'TPL').mkdir(parents=True, exist_ok=True)
    for n in (a.only or range(a.first, a.last + 1)):
        img = render(n, 'green' if n <= a.green_until else 'blue')
        img.save(a.out / 'PNG' / f'rank{n:03d}.png')
        (a.out / 'TPL' / f'rank{n:03d}.tpl').write_bytes(tpl_rgb5a3(img))
    print('done')

if __name__ == '__main__':
    main()
