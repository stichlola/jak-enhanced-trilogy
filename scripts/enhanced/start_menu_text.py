"""The start menu's lettering in the manner of Jak 3's logo: our own text (not the games' logos), drawn here.

    python scripts/enhanced/start_menu_text.py [--check]

Heavy letters leaning forward with rough, brushed edges (the masks warped by smooth noise), filled
with fire (yellow at the top through orange to deep red), dark cracks across it, a near-black
outline with a thin blue-violet rim outside it, and a soft shadow. Written to game/assets/enhanced/wheel/: title.png
("JAK ENHANCED TRILOGY") and name1..3.png (the cards' names), loaded by the runtime's start menu.
--check also writes local/start_menu_text_check.png.
"""
import argparse
import os
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / "game/assets/enhanced/wheel"
FONT_DIR = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"
SHEAR = 0.18  # the forward lean


def font(size):
    for n in ("ariblk.ttf", "impact.ttf", "arialbd.ttf"):
        if (FONT_DIR / n).exists():
            return ImageFont.truetype(str(FONT_DIR / n), size)
    return ImageFont.load_default(size)


def ramp(h, stops):
    """a vertical color ramp, stops: (position 0..1, (r, g, b))"""
    t = np.linspace(0, 1, h)
    pos = [p for p, _ in stops]
    return np.stack([np.interp(t, pos, [c[i] for _, c in stops]) for i in range(3)], -1)


FIRE = [(0.0, (255, 238, 120)), (0.25, (255, 196, 60)), (0.5, (246, 112, 26)),
        (0.78, (214, 44, 18)), (1.0, (150, 16, 14))]


def smooth_noise(h, w, scale, rng):
    """noise -1..1, smooth over about `scale` pixels"""
    small = rng.normal(0, 1, (max(2, h // scale + 2), max(2, w // scale + 2))).astype(np.float32)
    im = Image.fromarray(small).resize((w, h), Image.BICUBIC)
    a = np.asarray(im, np.float32)
    return a / (np.abs(a).max() + 1e-6)


def warp(mask, dx, dy):
    """the mask moved by the fields dx, dy (pixels)"""
    m = np.asarray(mask, np.float32)
    h, w = m.shape
    yy, xx = np.mgrid[0:h, 0:w]
    sx = np.clip(np.round(xx + dx).astype(int), 0, w - 1)
    sy = np.clip(np.round(yy + dy).astype(int), 0, h - 1)
    return Image.fromarray(m[sy, sx].astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8))


def lettering(parts, size):
    """parts: [(text, ramp stops)], set on one line; RGBA, cropped"""
    f = font(size)
    stroke = max(4, size // 11)
    probe = ImageDraw.Draw(Image.new("L", (1, 1)))
    full = "".join(t for t, _ in parts)
    box = probe.textbbox((0, 0), full, font=f, stroke_width=stroke)
    pad = stroke * 3
    w, h = box[2] - box[0] + pad * 2 + int(size * SHEAR), box[3] - box[1] + pad * 2
    origin = (pad - box[0], pad - box[1])
    fill = np.zeros((h, w, 3), np.float32)
    fill_mask = Image.new("L", (w, h))
    edge_mask = Image.new("L", (w, h))
    x = origin[0]
    top, bottom = origin[1] + box[1] + stroke, origin[1] + box[3] - stroke
    for text, stops in parts:
        m = Image.new("L", (w, h))
        ImageDraw.Draw(m).text((x, origin[1]), text, font=f, fill=255)
        ImageDraw.Draw(edge_mask).text((x, origin[1]), text, font=f, fill=255, stroke_width=stroke)
        col = np.zeros((h, 3), np.float32)
        col[top:bottom] = ramp(bottom - top, stops)
        col[:top], col[bottom:] = col[top], col[bottom - 1]
        a = np.asarray(m, np.float32)[..., None] / 255.0
        fill = fill * (1 - a) + col[:, None, :] * a
        fill_mask = Image.fromarray(np.maximum(np.asarray(fill_mask), np.asarray(m)))
        x += probe.textlength(text, font=f)
    # rough brushed edges: both masks pushed around by the same smooth noise, a finer one on top
    rng = np.random.default_rng(size)
    amp, fine = size * 0.035, size * 0.012
    dx = smooth_noise(h, w, size // 5, rng) * amp + smooth_noise(h, w, size // 22, rng) * fine
    dy = smooth_noise(h, w, size // 5, rng) * amp + smooth_noise(h, w, size // 22, rng) * fine
    fill_mask, edge_mask = warp(fill_mask, dx, dy), warp(edge_mask, dx, dy)
    # cracks: thin dark lines where a ridged noise is near zero, and a hot glow mottling the fire
    ridge = np.abs(smooth_noise(h, w, size // 6, rng) + 0.5 * smooth_noise(h, w, size // 14, rng))
    crack = np.clip(1 - ridge / 0.035, 0, 1) ** 1.5
    glow = smooth_noise(h, w, size // 8, rng) * 22
    fill = fill + glow[..., None] * np.array([1.0, 0.7, 0.2])
    fill = fill * (1 - 0.75 * crack[..., None]) + np.array([70, 10, 8]) * 0.75 * crack[..., None]
    fill = np.clip(fill, 0, 255)
    out = Image.new("RGBA", (w + 30, h + 30))
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    shadow.putalpha(edge_mask.filter(ImageFilter.GaussianBlur(size / 20)).point(lambda v: v * 0.75))
    out.alpha_composite(shadow, (size // 18, size // 14))
    # a thin blue-violet rim outside the dark outline
    rim_out = Image.new("RGBA", (w, h), (96, 70, 210, 255))
    rim_out.putalpha(edge_mask.filter(ImageFilter.MaxFilter(5)))
    out.alpha_composite(rim_out, (0, 0))
    edge = Image.new("RGBA", (w, h), (28, 6, 8, 255))
    edge.putalpha(edge_mask)
    out.alpha_composite(edge, (0, 0))
    # a thin light rim just inside the outline (the bevel's catch light)
    rim = Image.new("RGBA", (w, h), (255, 246, 200, 0))
    inner = fill_mask.filter(ImageFilter.MinFilter(5))
    rim.putalpha(Image.fromarray((np.asarray(fill_mask, np.float32) - np.asarray(inner, np.float32))
                                 .clip(0, 255).astype(np.uint8)).point(lambda v: v * 0.55))
    body = Image.fromarray(fill.astype(np.uint8), "RGB").convert("RGBA")
    body.putalpha(fill_mask)
    out.alpha_composite(body, (0, 0))
    out.alpha_composite(rim, (0, 0))
    # the forward lean (an affine shear about the baseline)
    out = out.transform(out.size, Image.AFFINE, (1, SHEAR, -SHEAR * out.height, 0, 1, 0),
                        resample=Image.BICUBIC)
    return out.crop(out.getbbox())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    DEST.mkdir(parents=True, exist_ok=True)
    pics = {
        "title": lettering([("JAK ENHANCED TRILOGY", FIRE)], 180),
        "name1": lettering([("JAK AND DAXTER", FIRE)], 120),
        "name2": lettering([("JAK II", FIRE)], 120),
        "name3": lettering([("JAK 3", FIRE)], 120),
    }
    for name, im in pics.items():
        im.save(DEST / f"{name}.png", optimize=True)
        print(DEST / f"{name}.png", im.size)
    if a.check:
        W = max(p.width for p in pics.values()) + 80
        H = sum(p.height + 40 for p in pics.values()) + 40
        bg = Image.new("RGBA", (W, H), (14, 20, 34, 255))
        y = 40
        for p in pics.values():
            bg.alpha_composite(p, ((W - p.width) // 2, y))
            y += p.height + 40
        out = ROOT / "local/start_menu_text_check.png"
        bg.convert("RGB").save(out)
        print(out)


if __name__ == "__main__":
    main()
