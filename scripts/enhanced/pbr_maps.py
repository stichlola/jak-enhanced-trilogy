"""Jak Enhanced Trilogy: normal and roughness maps for the HD textures (true PBR's companions).

    python scripts/enhanced/pbr_maps.py [--game jak1|jak2|jak3] [--jobs N] [--scale 0.5]

Runs after hd_textures.py: for each texture it upscaled (the same dump, the same cache keys) it reads
the 4x picture from out/enhanced-upscale-cache and writes one companion map,
out/enhanced/pbr/<game>/maps/<key>.png (RGB at --scale of the HD size, half by default):
  R, G  the tangent-space normal's x, y (x along u, y up the picture: v runs down it), z rebuilt
  B     roughness around 0.5: the shader adds it to its material class's own (0.5 = the class's)
The height behind the normal is the picture's brightness, high-passed (a band from a texel to a
sixteenth of the picture: the painted light across the whole texture isn't shape) and set to a common
contrast, so the shader's strength per material class means the same on every texture. Rougher where
the picture is busy and in its dark hollows, smoother on the raised plain parts.
index.txt lists "<tpage>/<name> <key>" for every texture that has one (texture names aren't unique:
the game also checks the map's size against the texture's). Done in parallel on the CPU, cached by
key: re-runs only make new ones. Prints 'pbr X/Y' as it goes (hd_setup.py's bar reads it).
"""

import argparse
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
import hd_textures as hd  # noqa: E402  (the same textures, the same keys)

VERSION = 1  # (bump when the maps change: older ones are made again)


def make_map(src, dst, scale):
    from scipy import ndimage
    img = Image.open(src).convert("RGBA")
    w, h = max(4, round(img.width * scale)), max(4, round(img.height * scale))
    img = img.resize((w, h), Image.BOX)
    a = np.asarray(img).astype(np.float32) / 255.0
    lum = a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11
    # (a cutout's see-through texels hold any color: their brightness isn't shape)
    alpha = a[..., 3]
    if alpha.min() < 0.45:
        mean = float((lum * alpha).sum() / max(alpha.sum(), 1.0))
        lum = lum * alpha + mean * (1.0 - alpha)
    big = max(2.0, min(w, h) / 16.0)
    fine = ndimage.gaussian_filter(lum, 0.6, mode="wrap")
    height = fine - ndimage.gaussian_filter(lum, big, mode="wrap")
    # a common contrast: the spread of the detail set to 1 (a flat texture stays nearly flat)
    spread = float(np.sqrt(np.mean(height * height)))
    height = height / max(spread, 0.035)
    # Sobel (wrapped: tiling textures stay seamless), per texel of this map
    gx = ndimage.sobel(height, axis=1, mode="wrap") / 8.0
    gy = ndimage.sobel(height, axis=0, mode="wrap") / 8.0
    # (x along u, y up the picture: rows run down it)
    k = 1.2
    nx, ny = -gx * k, gy * k
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + 1.0)
    nx, ny = nx * inv, ny * inv
    # roughness about the class's own: rougher where it's busy (the local spread of the detail) and
    # in the hollows, smoother on the raised plain parts
    local = np.sqrt(ndimage.gaussian_filter(height * height, 2.0, mode="wrap"))
    busy = np.tanh((local - 1.0) * 0.8)
    rough = 0.5 + 0.18 * busy - 0.14 * np.tanh(ndimage.gaussian_filter(height, 1.0, mode="wrap") * 0.8)
    out = np.dstack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, np.clip(rough, 0.0, 1.0)])
    Image.fromarray((out * 255.0 + 0.5).clip(0, 255).astype(np.uint8), "RGB").save(dst, compress_level=6)


def work(job):
    src, dst, scale = job
    try:
        tmp = dst.with_suffix(".tmp.png")
        make_map(src, tmp, scale)
        os.replace(tmp, dst)
        return None
    except Exception as e:  # (one bad picture doesn't stop the rest)
        return f"{src.name}: {e}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", choices=hd.GAMES, default="jak3")
    parser.add_argument("--model", default="realesrgan-x4plus")
    parser.add_argument("--min-size", type=int, default=16)
    parser.add_argument("--max-size", type=int, default=256)
    parser.add_argument("--scale", type=float, default=0.5, help="the map's size, of the HD texture's")
    parser.add_argument("--jobs", type=int, default=max(1, min(8, (os.cpu_count() or 2) - 2)))
    parser.add_argument("--limit", type=int, default=0, help="(testing: only this many)")
    parser.add_argument("--out", type=Path, help="(where the maps go: default out/enhanced/pbr/<game>)")
    parser.add_argument("--root", type=Path, help="(the project with the dump and the cache: a worktree's main tree)")
    args = parser.parse_args()
    global ROOT
    if args.root:
        ROOT = hd.ROOT = args.root.resolve()
        hd.CACHE = ROOT / "out/enhanced-upscale-cache"

    dump = hd.find_dump(args.game)
    skip = re.compile(hd.EXCLUDE)
    cache = hd.CACHE / args.model
    out = args.out or ROOT / "out" / "enhanced" / "pbr" / args.game
    maps = out / "maps"
    maps.mkdir(parents=True, exist_ok=True)
    stamp = f"v{VERSION}-s{args.scale:g}"
    stamp_file = maps / "version.txt"
    if not stamp_file.exists() or stamp_file.read_text().strip() != stamp:
        for old in maps.glob("*.png"):
            old.unlink()
        stamp_file.write_text(stamp)

    import hashlib
    index = {}
    for tpage in sorted(p for p in dump.iterdir() if p.is_dir()):
        if skip.search(tpage.name):
            continue
        for png in sorted(tpage.glob("*.png")):
            if skip.search(png.stem):
                continue
            data = png.read_bytes()
            w, h = hd.png_size(data)
            if min(w, h) < args.min_size or max(w, h) > args.max_size:
                continue
            key = hashlib.sha1(data).hexdigest()
            if (cache / f"{key}.png").exists():
                index[f"{tpage.name}/{png.stem}"] = key
    keys = sorted(set(index.values()))
    if args.limit:
        keys = keys[:args.limit]
    todo = [(cache / f"{k}.png", maps / f"{k}.png", args.scale) for k in keys if not (maps / f"{k}.png").exists()]
    print(f"pbr maps: {len(index)} textures, {len(keys)} pictures, {len(todo)} to make", flush=True)
    done = 0
    errors = []
    if todo:
        with ProcessPoolExecutor(args.jobs) as pool:
            for err in pool.map(work, todo, chunksize=16):
                done += 1
                if err:
                    errors.append(err)
                if done % 200 == 0 or done == len(todo):
                    print(f"  pbr {done}/{len(todo)}", flush=True)
    for e in errors[:10]:
        print("  error:", e)
    have = set(k for k in keys if (maps / f"{k}.png").exists())
    lines = [f"{name} {key}" for name, key in sorted(index.items()) if key in have]
    (out / "index.txt").write_text("\n".join(lines) + "\n")
    size = sum(f.stat().st_size for f in maps.glob("*.png"))
    print(f"wrote {len(lines)} entries to {out / 'index.txt'}, maps {size / 2**20:.0f} MB", flush=True)


if __name__ == "__main__":
    sys.exit(main())
