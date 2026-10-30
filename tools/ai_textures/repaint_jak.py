#!/usr/bin/env python3
"""Jak 3: the textures of Jak that the 4x upscale spoiled, remade from the game's own, into texture_remade.

The upscale took the pictures' single-pixel mottling for shapes and drew it as blocks with stepped
edges. Here the originals only give the layout and the colors. The shoes are painted new (where the
leather is worn through, where the plate is, the color of each), the others keep the original's
shapes without its pixels. All at 8x, each with a height map (<name>_h.png) so that the relief
follows the material and not the painted marks.

    python repaint_jak.py <decompiler_out>/jak3/textures <out>/texture_remade
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

SCALE = 8
PAGE = "level-default-pris"


def blur(a, sigma):
    """Gaussian blur, edges extended (not wrapped: the pictures don't tile)."""
    if sigma <= 0:
        return a.copy()
    pad = int(sigma * 3) + 1
    if a.ndim == 3:
        return np.stack([blur(a[:, :, c], sigma) for c in range(a.shape[2])], axis=2)
    p = np.pad(a, pad, mode="edge")
    fy = np.fft.fftfreq(p.shape[0])[:, None]
    fx = np.fft.rfftfreq(p.shape[1])[None, :]
    g = np.exp(-2.0 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy))
    return np.fft.irfft2(np.fft.rfft2(p) * g, s=p.shape)[pad:-pad, pad:-pad]


def noise(shape, sigma, rng):
    """Smooth noise of about unit variance."""
    n = blur(rng.standard_normal(shape), sigma)
    return n / max(n.std(), 1e-6)


def grain(shape, rng):
    """Leather: fine pores over soft unevenness."""
    return 0.5 * noise(shape, 1.1, rng) + 0.35 * noise(shape, 3.0, rng) + 0.5 * noise(shape, 12.0, rng)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def masked_color(rgb, mask, sigma):
    """The color of the masked part, spread smoothly over the whole picture."""
    w = blur(mask, sigma)[:, :, None] + 1e-3
    return blur(rgb * mask[:, :, None], sigma) / w


def load(folder, name):
    im = Image.open(folder / f"{name}.png").convert("RGBA")
    size = (im.width * SCALE, im.height * SCALE)
    rgb = np.asarray(im.convert("RGB").resize(size, Image.BICUBIC), float) / 255.0
    # (the PS2's alpha, 128 = opaque, kept as it is)
    alpha = np.asarray(im.getchannel("A").resize(size, Image.BICUBIC), float)
    return np.clip(rgb, 0.0, 1.0), alpha


def save(folder, name, rgb, alpha, height):
    folder.mkdir(parents=True, exist_ok=True)
    out = np.dstack([np.clip(rgb, 0.0, 1.0) * 255.0, alpha]).round().astype("uint8")
    Image.fromarray(out, "RGBA").save(folder / f"{name}.png")
    h = (np.clip(height, 0.0, 1.0) * 255.0).round().astype("uint8")
    Image.fromarray(h, "L").save(folder / f"{name}_h.png")


def rounded_rect(shape, rect, radius, soft):
    """A rectangle's coverage (1 inside) and the signed distance to its edge (negative inside)."""
    H, W = shape
    y, x = np.mgrid[0:H, 0:W].astype(float)
    x0, y0, x1, y1 = rect
    cx, cy = (x0 + x1) * 0.5, (y0 + y1) * 0.5
    hx, hy = (x1 - x0) * 0.5 - radius, (y1 - y0) * 0.5 - radius
    qx, qy = np.abs(x - cx) - hx, np.abs(y - cy) - hy
    d = np.hypot(np.maximum(qx, 0.0), np.maximum(qy, 0.0)) + np.minimum(np.maximum(qx, qy), 0.0) - radius
    return 1.0 - smoothstep(-soft, soft, d), d


def shoe_top(src, rng):
    rgb, alpha = load(src, "jakc-shoetop")
    shape = rgb.shape[:2]
    # (the pale plate at the toe, where the original has it, in its pixels)
    rect = tuple(v * SCALE for v in (22, 100, 42, 128))
    plate, dist = rounded_rect(shape, rect, SCALE * 0.8, 1.5)

    # worn through: where the original is warmer than blue, its mottling averaged into shapes, the
    # edges broken up by noise (chipped, not stepped)
    soft = blur(rgb, SCALE * 1.1)
    warm = soft[:, :, 0] - soft[:, :, 2]
    field = warm + 0.035 * noise(shape, SCALE * 0.9, rng) + 0.012 * noise(shape, 2.0, rng)
    worn = smoothstep(0.0, 0.012, field) * (1.0 - plate)
    # (scuffs: thin scratches along the shoe, faint, most near what's already worn)
    scratches = blur(rng.standard_normal(shape), 1.0)
    scratches = np.fft.irfft(np.fft.rfft(scratches, axis=1) * np.exp(-((np.fft.rfftfreq(shape[1]) * 40.0) ** 2))[None, :], n=shape[1], axis=1)
    scratches = smoothstep(1.6, 2.6, scratches / scratches.std())

    blue = masked_color(rgb, (warm < -0.01).astype(float) * (1.0 - plate), SCALE * 6.0)
    tan = masked_color(rgb, (warm > 0.02).astype(float) * (1.0 - plate), SCALE * 6.0)
    g = grain(shape, rng)[:, :, None]
    # (the blue's own wear: paler rubbed areas as in the original, broad)
    rubbed = blur(rgb, SCALE * 2.5).mean(axis=2, keepdims=True) / np.maximum(blur(rgb, SCALE * 12.0).mean(axis=2, keepdims=True), 0.02)
    leather = blue * np.clip(rubbed, 0.75, 1.3) * (1.0 + 0.05 * g)
    bare = tan * (1.0 + 0.09 * g)
    out = leather * (1.0 - worn[:, :, None]) + bare * worn[:, :, None]
    # (a dark line where the paint's edge stands over the bare part)
    edge = np.clip(blur(worn, 2.5) - worn, 0.0, 1.0)[:, :, None]
    out *= 1.0 - 0.9 * edge
    out = out + (tan - out) * scratches[:, :, None] * 0.25

    # the plate: one flat piece, a little brighter at the top, its edge bevelled, a shadow around it
    plate_color = rgb[rect[1] + SCALE : rect[3] - SCALE, rect[0] + SCALE : rect[2] - SCALE].mean(axis=(0, 1))
    plate_rgb = plate_color[None, None, :] * (1.0 + 0.035 * g)
    bevel = (1.0 - smoothstep(-4.0, -1.0, dist)) * smoothstep(-9.0, -4.0, dist)
    plate_rgb = plate_rgb * (1.0 + 0.12 * bevel[:, :, None])
    shadow = (1.0 - smoothstep(0.0, 8.0, dist)) * (1.0 - plate)
    out *= 1.0 - 0.45 * shadow[:, :, None]
    out = out * (1.0 - plate[:, :, None]) + plate_rgb * plate[:, :, None]

    h = 0.5 + 0.035 * g[:, :, 0] - 0.12 * worn - 0.05 * scratches + 0.28 * plate
    return out, alpha, h


def shoe_plate(src, rng):
    rgb, alpha = load(src, "jakc-shoeplate")
    shape = rgb.shape[:2]
    # its lengthwise bands: the picture averaged down each column for their sharp lines, the slow
    # change along them from the picture itself
    soft = blur(rgb, SCALE * 2.5)
    columns = rgb.mean(axis=0, keepdims=True)
    detail = columns / np.maximum(blur(columns, SCALE * 2.0), 0.02)
    g = grain(shape, rng)[:, :, None]
    out = soft * np.clip(detail, 0.7, 1.25) ** 1.4 * (1.0 + 0.05 * g)
    groove = np.clip(1.0 - detail.mean(axis=2), 0.0, 1.0)
    h = 0.5 + 0.03 * g[:, :, 0] - 1.2 * groove
    return out, alpha, h


def shoe_bottom(src, rng):
    rgb, alpha = load(src, "jakc-shoebottom")
    shape = rgb.shape[:2]
    # the sole: a pale rim around a darker worn middle, nothing else (the upscale drew cracks and
    # figures into the mottling)
    soft = blur(rgb, SCALE * 2.0)
    lum = soft.mean(axis=2)
    field = lum + 0.012 * noise(shape, SCALE * 0.8, rng)
    split = 0.5 * (np.percentile(lum, 15) + np.percentile(lum, 85))
    rim = smoothstep(split - 0.012, split + 0.012, field)
    pale = masked_color(rgb, (lum > split).astype(float), SCALE * 8.0)
    dark = masked_color(rgb, (lum <= split).astype(float), SCALE * 8.0)
    g = grain(shape, rng)[:, :, None]
    out = (dark * (1.0 - rim[:, :, None]) + pale * rim[:, :, None]) * (1.0 + 0.07 * g)
    h = 0.5 + 0.04 * g[:, :, 0] + 0.15 * rim
    return out, alpha, h


def smooth(src, name, rng):
    """The original's own shapes without its pixels: smoothed past the grid, the edges tightened
    again (no steps, and nothing the original doesn't have), a little new grain."""
    rgb, alpha = load(src, name)
    shape = rgb.shape[:2]
    soft = blur(rgb, SCALE * 0.6)
    out = soft + 0.9 * (soft - blur(soft, SCALE * 1.6))
    g = grain(shape, rng)
    out = out * (1.0 + 0.045 * g[:, :, None])
    lum = soft.mean(axis=2)
    h = 0.5 + 1.2 * (lum - blur(lum, SCALE * 4.0)) + 0.03 * g
    return out, alpha, h


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("textures", type=Path, help="the decompiler's textures folder")
    ap.add_argument("out", type=Path, help="texture_remade folder")
    args = ap.parse_args()
    src = args.textures / PAGE
    dst = args.out / PAGE
    painters = [("jakc-shoetop", shoe_top), ("jakc-shoeplate", shoe_plate), ("jakc-shoebottom", shoe_bottom)]
    # (those the upscale gave steps, or marks the original doesn't have: the waistband's lines, a
    # corner of the fingers. Not the hair: its black spots are see-through, they don't show)
    for name in ("waistband", "leggging", "leatherpouch", "glovetop", "wraps", "wristbands-a", "finger"):
        painters.append((f"jakc-{name}", lambda s, r, n=f"jakc-{name}": smooth(s, n, r)))
    for name, paint in painters:
        rng = np.random.default_rng(sum(name.encode()))
        rgb, alpha, height = paint(src, rng)
        save(dst, name, rgb, alpha, height)
        print("wrote", dst / f"{name}.png", rgb.shape[1], "x", rgb.shape[0])


if __name__ == "__main__":
    main()
