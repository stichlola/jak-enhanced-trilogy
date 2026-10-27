"""Jak 3's pause maps, redrawn by Tripo as parchment (bigmap_tripo.py), made ready for the game.

    python scripts/enhanced/bigmap_hd.py [--check]

Reads local/bigmap/mapNN.png (the game's maps, for their size only) and local/bigmap/tripo/mapNN_raw.png
(Tripo's drawings, 2048 x 2048) and writes game/assets/enhanced/bigmap/jak3/mapNN.jpg at 4 x the
game's size (each map is w x h pixels, uploaded by the game 256 x 416 at a time; the runtime cuts the
same window out of these). Maps 01 and 09 are empty placeholders in the game (Tripo invented a whole
world for them): they get a plain parchment drawn here. Map 14 had an island added below its strip:
that band is painted over with the parchment from the top of the same picture.
--check also writes local/bigmap/hd_check.png (all of them, small).
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "local/bigmap"
DEST = ROOT / "game/assets/enhanced/bigmap/jak3"
SCALE = 4
PLAIN = (1, 9)


def parchment(w, h, seed):
    """aged paper: a warm base, soft blotches, a fine grain, darker toward the edges"""
    rng = np.random.default_rng(seed)
    base = np.array([226, 206, 160], np.float32)

    def blotch(scale, amp):
        small = rng.normal(0, 1, (max(2, h // scale), max(2, w // scale))).astype(np.float32)
        return np.asarray(Image.fromarray(small).resize((w, h), Image.BICUBIC), np.float32) * amp

    tone = blotch(160, 7) + blotch(40, 4) + rng.normal(0, 3, (h, w))
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    edge = np.minimum(np.minimum(x, w - 1 - x) / w, np.minimum(y, h - 1 - y) / h)
    vignette = np.clip(edge / 0.12, 0, 1) ** 0.6
    rgb = base[None, None, :] * (0.78 + 0.22 * vignette[..., None]) + tone[..., None] * np.array([1.0, 0.9, 0.7])
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB").filter(ImageFilter.GaussianBlur(0.6))


def fix_14(im):
    """the game's map 14 is one long strip and its lettering on an empty sheet; Tripo drew land
    above and below them. Only the band of the strip and the lettering is kept (rows measured on
    Tripo's picture), the rest becomes parchment in the tone of Tripo's own paper"""
    a = np.asarray(im, np.float32)
    h, w = a.shape[:2]
    y = np.arange(h, dtype=np.float32) / h
    soft = 0.025
    keep = np.clip((y - 0.375) / soft + 0.5, 0, 1) * np.clip((0.635 - y) / soft + 0.5, 0, 1)
    x = np.arange(w, dtype=np.float32) / w
    keep = keep[:, None] * np.clip((x - 0.05) / 0.03 + 0.5, 0, 1)[None, :] * np.clip((0.97 - x) / 0.03 + 0.5, 0, 1)[None, :]
    keep = keep[..., None]
    # the paper's tone: Tripo's warm pixels (red over green over blue: not the washes of water)
    px = a.reshape(-1, 3)
    warm = px[(px[:, 0] > px[:, 1]) & (px[:, 1] > px[:, 2]) & (px[:, 0] > 170)]
    paper = np.asarray(parchment(w, h, seed=14), np.float32)
    paper = paper * (np.median(warm, axis=0) / np.median(paper.reshape(-1, 3), axis=0))
    a = a * keep + paper * (1 - keep)
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), "RGB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    DEST.mkdir(parents=True, exist_ok=True)
    thumbs = []
    for n in range(24):
        w, h = Image.open(SRC / f"map{n:02d}.png").size
        size = (w * SCALE, h * SCALE)
        if n in PLAIN:
            im = parchment(*size, seed=n)
        else:
            # (Tripo answered square: back to the map's own proportions, as bigmap_tripo.py did)
            im = Image.open(SRC / f"tripo/map{n:02d}_raw.png").convert("RGB").resize(size, Image.LANCZOS)
            if n == 14:
                im = fix_14(im)
        out = DEST / f"map{n:02d}.jpg"
        im.save(out, quality=88, optimize=True, progressive=False)
        print(out, size, f"{out.stat().st_size // 1024} KB")
        thumbs.append(im.resize((300, 300 * h // w)))
    if args.check:
        sheet = Image.new("RGB", (6 * 300, max(t.height for t in thumbs) * 4))
        row_h = sheet.height // 4
        for i, t in enumerate(thumbs):
            sheet.paste(t, ((i % 6) * 300, (i // 6) * row_h))
        sheet.save(SRC / "hd_check.png")
        print(SRC / "hd_check.png")


if __name__ == "__main__":
    main()
