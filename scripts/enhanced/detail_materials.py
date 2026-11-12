"""Tileable PBR detail materials for the level shaders (tfrag3.frag, hfrag.frag).

Each material is one small RGBA picture laid in world space over a surface class (the game's own
texture keeps the color): R G a tangent-space normal (x along world X, y along world Z; the
gradient of the height, R = -dh/du, G = -dh/dv with v down the rows), B a roughness offset about
the class's own (0.5 = none), A ambient occlusion (1 = open). The source pictures are our own
generated art; the outputs go to game/assets/enhanced/materials/ (in git, kept small).

    python scripts/enhanced/detail_materials.py [--only sand] [--preview <dir>]

Steps per material: the seam test (wrap-pad: the step across the picture's edges against the steps
inside it; above 1.2 the edges are blended across a band so it tiles), height = brightness
high-passed at two scales (grain and ripples, each set to a common contrast), Sobel (wrapped) to a
normal, roughness from the grain's local spread (rougher where busy, smoother on ripple crests), AO
from the height under a blur of it (hollows darker).
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_DIR = os.path.join(ROOT, "game", "assets", "enhanced", "materials")

# name -> source picture, output size, and the shape of its relief
MATERIALS = {
    # fine wind-blown sand: grains, small ripples, a few pebbles (2048 seamless, generated art)
    "sand": {
        "src": [
            os.path.join(ROOT, "local", "higgsfield", "sand_pbr.png"),
            # (from a worktree: the main checkout's local/, beside jak-enhanced-wt)
            os.path.join(os.path.dirname(os.path.dirname(ROOT)), "jak-enhanced", "local", "higgsfield", "sand_pbr.png"),
        ],
        # (the 2048 picture nearly repeats itself at half its width: its top-left quarter is the tile,
        # same texel size at a quarter of the file; detail.json tile_m is set to match, 0.9 m)
        "crop": 0.5,
        "size": 512,
        "grain_px": 0.6,    # the grain's high-pass, texels of the output
        "ripple_px": 20.0,  # the ripples' (a ripple is ~70 texels)
        "grain_w": 0.55,    # their mix in the height
        "ripple_w": 1.0,
        "normal_k": 1.6,    # Sobel gain
        "ao_px": 5.0,
        "ao_k": 0.22,
    },
}


def seam_ratio(a):
    seam = (np.abs(a[0] - a[-1]).mean() + np.abs(a[:, 0] - a[:, -1]).mean()) / 2
    inner = (np.abs(a[1:] - a[:-1]).mean() + np.abs(a[:, 1:] - a[:, :-1]).mean()) / 2
    return seam / max(inner, 1e-6)


def make_seamless(a, band=0.08):
    """Blend the picture with itself rolled by half across a band at each edge (wrap fix)."""
    h, w = a.shape[:2]
    r = np.roll(np.roll(a, h // 2, 0), w // 2, 1)
    def ramp(n):
        x = np.minimum(np.arange(n), n - 1 - np.arange(n)) / (n * band)
        return np.clip(x, 0.0, 1.0)
    m = np.minimum.outer(ramp(h), ramp(w))
    m = m * m * (3 - 2 * m)
    if a.ndim == 3:
        m = m[..., None]
    return a * m + r * (1 - m)


def build(name, cfg, preview_dir=None):
    src = next((p for p in cfg["src"] if os.path.exists(p)), None)
    if src is None:
        print(f"{name}: no source picture ({cfg['src'][0]})", file=sys.stderr)
        return False
    img = Image.open(src).convert("RGB")
    a = np.asarray(img).astype(np.float32) / 255.0
    if cfg.get("crop", 1.0) < 1.0:
        c = int(round(a.shape[0] * cfg["crop"]))
        a = a[:c, :c]
    ratio = seam_ratio(a)
    if ratio > 1.2:
        a = make_seamless(a)
        print(f"{name}: seam ratio {ratio:.2f} -> {seam_ratio(a):.2f} (blended)")
    else:
        print(f"{name}: seam ratio {ratio:.2f} (seamless)")
    n = cfg["size"]
    a = np.asarray(Image.fromarray((a * 255 + 0.5).clip(0, 255).astype(np.uint8)).resize((n, n), Image.LANCZOS))
    a = a.astype(np.float32) / 255.0
    lum = a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11

    def band(lo, hi):
        h = ndimage.gaussian_filter(lum, lo, mode="wrap") - ndimage.gaussian_filter(lum, hi, mode="wrap")
        return h / max(float(np.sqrt(np.mean(h * h))), 1e-4)

    grain = band(min(0.5, cfg["grain_px"] * 0.5), cfg["grain_px"] * 3.0)
    ripple = band(cfg["grain_px"] * 3.0, cfg["ripple_px"])
    height = grain * cfg["grain_w"] + ripple * cfg["ripple_w"]
    height /= float(np.sqrt(np.mean(height * height)))
    gx = ndimage.sobel(height, axis=1, mode="wrap") / 8.0
    gy = ndimage.sobel(height, axis=0, mode="wrap") / 8.0
    k = cfg["normal_k"]
    nx, ny = -gx * k, -gy * k
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + 1.0)
    nx, ny = nx * inv, ny * inv
    # roughness: rougher where the grain is busy, a little smoother on the ripples' crests
    local = np.sqrt(ndimage.gaussian_filter(grain * grain, 3.0, mode="wrap"))
    rough = 0.5 + 0.2 * np.tanh((local - 1.0) * 1.2) - 0.1 * np.tanh(ndimage.gaussian_filter(ripple, 2.0, mode="wrap"))
    # AO: how far a texel sits under the blurred height around it
    cav = ndimage.gaussian_filter(height, cfg["ao_px"], mode="wrap") - height
    ao = 1.0 - cfg["ao_k"] * np.clip(cav, 0.0, None)
    # (never black: a pebble's dark rim isn't a pit)
    ao = np.clip(ndimage.gaussian_filter(ao, 0.8, mode="wrap"), 0.55, 1.0)
    out = np.dstack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, np.clip(rough, 0, 1), ao])
    out8 = (out * 255.0 + 0.5).clip(0, 255).astype(np.uint8)
    os.makedirs(OUT_DIR, exist_ok=True)
    dst = os.path.join(OUT_DIR, f"{name}_detail.png")
    Image.fromarray(out8, "RGBA").save(dst, optimize=True)
    print(f"{name}: {dst} {n}x{n}, {os.path.getsize(dst) // 1024} KB; "
          f"normal xy spread {float(np.std(nx)):.3f}, rough {float(rough.min()):.2f}..{float(rough.max()):.2f}, "
          f"ao min {float(ao.min()):.2f} mean {float(ao.mean()):.3f}")
    if preview_dir:
        os.makedirs(preview_dir, exist_ok=True)
        # the normal lit from the upper left, 2x2 (seams show), and the four channels
        lit = np.clip(0.5 + (-nx * 0.6 + ny * 0.6) * 1.5, 0, 1) * ao
        tile = np.tile(lit, (2, 2))
        Image.fromarray((tile * 255).astype(np.uint8)).resize((768, 768)).save(os.path.join(preview_dir, f"{name}_lit.jpg"))
        chans = np.concatenate([out[..., 0], out[..., 1], out[..., 2], out[..., 3]], 1)
        Image.fromarray((chans * 255).astype(np.uint8)).resize((1024, 256)).save(os.path.join(preview_dir, f"{name}_chans.jpg"))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--preview")
    args = ap.parse_args()
    ok = True
    for name, cfg in MATERIALS.items():
        if args.only and name not in args.only:
            continue
        ok = build(name, cfg, args.preview) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
