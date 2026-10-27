"""The district maps of the minimap and of the big map, redrawn at 4x in a modern style (Jak II and 3).

  python scripts/enhanced/minimap_maps.py [--only <name>...] [--preview]

The maps are the textures "map-..." of the "...-minimap" pages, from the decompiler's texture dump.
Most are flat 4-bit drawings (Haven's tiles: dark blocks, white streets, blue water, green parks);
they keep their exact shapes (each color region's edge smoothed at 4x) and get a new look: a calmer
palette, raised blocks with a lit bevel and a soft shadow on the streets, thin outlines, water deeper
away from the shore with a light rim. The painted ones (the desert, Spargus...) are enlarged with
Real-ESRGAN (find_esrgan in hd_textures.py). The results go where the game loads them,
out/enhanced/hud/<game>/<tpage>/<name>.png (EnhancedMapTextures.cpp): made from the games' art,
so not in git. --preview also writes local/minimap_maps/preview.png, before and after.
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from hd_textures import find_dump, find_esrgan  # noqa: E402
OUT = ROOT / "out" / "enhanced" / "hud"
S = 4  # the game's kEnhancedMapScale

# the restyled palette
VOID = np.array([16, 22, 30], float)     # outside the map
SLATE = np.array([52, 60, 76], float)    # the blocks, for the game's (57, 57, 57)
WATER = np.array([28, 128, 214], float)  # pulled toward this


def load(path):
    a = np.asarray(Image.open(path).convert("RGBA")).astype(np.float64)
    a[..., 3] = np.minimum(255, a[..., 3] * 2)  # the PS2's alpha (128 opaque) to 255
    return a


def save(a, path):
    a = a.copy()
    a[..., 3] = np.round(a[..., 3] / 2)
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.clip(np.round(a), 0, 255).astype(np.uint8), "RGBA").save(path)


def role(c):
    r, g, b = c[:3]
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    sat = max(r, g, b) - min(r, g, b)
    if lum < 20:
        return "void"
    if sat < 30:
        return "block" if lum < 110 else "street"
    if b > r + 60 and b >= g:
        return "water"
    return "feature"


def recolor(c, kind):
    rgb = np.array(c[:3], float)
    lum = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]
    if kind == "void":
        return VOID
    if kind == "block":
        return np.clip(SLATE * (lum / 57.0), 0, 255)
    if kind == "street":
        return np.clip(np.array([lum * 0.93 + 4, lum * 0.94 + 6, lum * 0.95 + 10]), 0, 255)
    if kind == "water":
        return rgb * 0.5 + WATER * 0.5
    return rgb * 0.92 + lum * 0.08  # parks and the rest: a touch calmer


def blur(x, sigma):
    return ndimage.gaussian_filter(x, sigma, mode="nearest")


def flat(a):
    h, w = a.shape[:2]
    colors, labels = np.unique(a.reshape(-1, 4), axis=0, return_inverse=True)
    labels = labels.reshape(h, w)
    # each color's region, its edge smoothed: the color that covers the most of each new pixel
    cover = np.stack([ndimage.zoom(blur((labels == k).astype(float), 0.7), S, order=1, mode="nearest",
                                   grid_mode=True) for k in range(len(colors))])
    big = np.argmax(cover, axis=0)
    kinds = [role(c) for c in colors]
    rgb = np.stack([recolor(c, k) for c, k in zip(colors, kinds)])[big]
    alpha = colors[:, 3][big]
    kind = np.array(kinds)[big]

    block = (kind == "block").astype(float)
    low = ~((kind == "block") | (kind == "void"))
    # the blocks stand up: lit from the top left, a soft shadow to the bottom right on what is low
    height = blur(block, 1.6 * S / 4)
    gy, gx = np.gradient(height)
    bevel = np.clip(-(gx + gy) * 2.2, -0.35, 0.35)
    rgb[kind == "block"] *= (1 + bevel[kind == "block"])[:, None]
    shadow = blur(np.roll(np.roll(block, 3, 0), 3, 1), 2.5)
    rgb[low] *= (1 - 0.38 * shadow[low])[:, None]
    # the water darker away from the shore, a light rim along it
    water = kind == "water"
    if water.any():
        depth = ndimage.distance_transform_edt(water)
        rgb[water] *= (1 - 0.3 * np.minimum(depth[water] / (12 * S), 1))[:, None]
        rim = water & (depth <= 1.5 * S / 4 + 1)
        rgb[rim] = rgb[rim] * 0.6 + 255 * 0.4
    # thin outlines where two kinds meet (not inside a kind's own shades)
    edge = np.zeros_like(block, bool)
    for axis in (0, 1):
        d = np.diff(big, axis=axis) != 0
        kd = np.diff((kind == "block").astype(int) + 2 * (kind == "street") + 4 * water + 8 * (kind == "void"),
                     axis=axis) != 0
        e = d & kd
        if axis == 0:
            edge[:-1] |= e
            edge[1:] |= e
        else:
            edge[:, :-1] |= e
            edge[:, 1:] |= e
    rgb[edge] *= 0.78
    # a faint grain, so the flat areas look printed rather than filled
    rng = np.random.default_rng(7)
    rgb += rng.normal(0, 1.6, rgb.shape) * (kind != "void")[..., None]
    return np.dstack([np.clip(rgb, 0, 255), alpha])


def painted(items, tmp):
    """Real-ESRGAN 4x on the color, the alpha enlarged smoothly"""
    src, dst = tmp / "in", tmp / "out"
    src.mkdir()
    dst.mkdir()
    for i, (a, _) in enumerate(items):
        Image.fromarray(a[..., :3].astype(np.uint8), "RGB").save(src / f"{i}.png")
    esrgan = find_esrgan()
    subprocess.run([str(esrgan), "-i", str(src), "-o", str(dst), "-n", "realesrgan-x4plus", "-s", "4",
                    "-m", str(esrgan.parent / "models"), "-f", "png"], check=True, capture_output=True)
    out = []
    for i, (a, path) in enumerate(items):
        rgb = np.asarray(Image.open(dst / f"{i}.png").convert("RGB")).astype(float)
        alpha = ndimage.zoom(a[..., 3], S, order=3, mode="nearest", grid_mode=True)
        out.append((np.dstack([rgb, np.clip(alpha, 0, 255)]), path))
    return out


def main():
    args = sys.argv[1:]
    only = set(args[args.index("--only") + 1:]) if "--only" in args else None
    maps = []
    for game in ("jak2", "jak3"):
        try:
            textures = find_dump(game)
        except SystemExit:
            continue  # that game isn't extracted
        for path in sorted(textures.glob("*-minimap/map-*.png")):
            if only and path.stem not in only:
                continue
            a = load(path)
            if min(a.shape[:2]) >= 64:
                maps.append((game, path, a))
    done, to_paint = [], []
    for game, path, a in maps:
        target = OUT / game / path.parent.name / path.name
        if len(np.unique(a.reshape(-1, 4), axis=0)) <= 16:
            done.append((a, flat(a), target))
        else:
            to_paint.append((a, target))
    if to_paint:
        with tempfile.TemporaryDirectory() as tmp:
            done += [(a, b, t) for (a, _), (b, t) in zip(to_paint, painted(to_paint, Path(tmp)))]
    for _, b, target in done:
        save(b, target)
    print(f"{len(done)} maps in {OUT}")
    if "--preview" in args:
        tile = 256
        sheet = Image.new("RGB", (tile * 2, tile * len(done)), (255, 0, 255))
        for i, (a, b, _) in enumerate(done):
            for j, img in enumerate((a, b)):
                im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA").resize((tile, tile),
                                                                                          Image.LANCZOS)
                sheet.paste(im.convert("RGB"), (j * tile, i * tile))
        (ROOT / "local" / "minimap_maps").mkdir(parents=True, exist_ok=True)
        sheet.save(ROOT / "local" / "minimap_maps" / "preview.png")


if __name__ == "__main__":
    main()
