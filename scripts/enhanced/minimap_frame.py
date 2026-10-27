"""The minimap's frame redrawn in high definition, the same for Jak II and Jak 3 (drawn here: no game art).

    python scripts/enhanced/minimap_frame.py [--check]

The games draw the minimap's ring with hud-mapring-01 and its alarm glow (the city on alert) with
hud-mapring-alarm-01, both 128 x 128 in the level-default-minimap tpage: a thin olive ring at 60-63
of the 64 pixels' radius, and a yellow-to-orange disc. Here, 1024 x 1024 each, the ring at the same
radius: a dark outer lip, a brushed metal band lit from the upper left, a fine bright inner edge,
four small notches, and a soft shadow falling inward over the map; the alarm the same disc, smooth.
Written where the game loads HUD overrides: game/assets/enhanced/hud/<game>/level-default-minimap/.
Alpha is in the PS2's scale (128 = opaque). --check also writes local/minimap_frame_check.png.
"""
import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SIZE = 1024
GAMES = ("jak2", "jak3")


def grid():
    c = (SIZE - 1) / 2.0
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32)
    dx, dy = (x - c) / (SIZE / 2.0), (y - c) / (SIZE / 2.0)
    return np.sqrt(dx * dx + dy * dy), np.arctan2(dy, dx)


def band(r, a, b, soft):
    """1 inside [a, b] (radius, 0..1 of the half size), soft edges"""
    return np.clip((r - a) / soft + 0.5, 0, 1) * np.clip((b - r) / soft + 0.5, 0, 1)


def ring():
    r, ang = grid()
    px = 1.0 / (SIZE / 2.0)
    rgb = np.zeros((SIZE, SIZE, 3), np.float32)
    alpha = np.zeros((SIZE, SIZE), np.float32)

    def over(color, a):
        nonlocal rgb, alpha
        a = np.clip(a, 0, 1)
        rgb = rgb * (1 - a[..., None]) + np.asarray(color, np.float32)[None, None, :] * a[..., None]
        alpha = alpha + a * (1 - alpha)

    # the shadow the ring casts inward over the map
    over((0, 0, 0), 0.45 * np.clip((r - 0.86) / 0.08, 0, 1) * (r < 0.935))
    # dark outer lip
    over((14, 15, 10), band(r, 0.935, 0.995, 2 * px))
    # the metal band: lit from the upper left, a fine brushed grain around it
    light = 0.5 + 0.5 * np.cos(ang - math.radians(-135))
    grain = 0.015 * np.sin(ang * 340.0) + 0.01 * np.sin(ang * 911.0 + 1.3)
    metal = np.stack([150 + 70 * light, 152 + 66 * light, 104 + 46 * light], -1) * (1 + grain[..., None])
    # rounded across its width: brighter in the middle of the band
    across = np.clip(1 - ((r - 0.962) / 0.022) ** 2, 0, 1)
    metal = metal * (0.55 + 0.45 * across[..., None])
    m = band(r, 0.944, 0.982, 1.5 * px)
    rgb = rgb * (1 - m[..., None]) + metal * m[..., None]
    alpha = alpha + m * (1 - alpha)
    # a fine bright inner edge
    over((235, 232, 190), 0.8 * band(r, 0.941, 0.947, px) * (0.4 + 0.6 * light))
    # four small notches (up, down, left, right), darker cuts across the band
    for k in range(4):
        d = np.abs(np.angle(np.exp(1j * (ang - k * math.pi / 2))))
        over((20, 20, 14), 0.8 * band(r, 0.946, 0.98, px) * np.clip((0.012 - d) / 0.004, 0, 1))
    out = np.dstack([np.clip(rgb, 0, 255), alpha * 128.0]).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def alarm():
    r, _ = grid()
    # the game's disc: yellow (255, 207, 0) in the middle to orange-red (255, 60, 0) at the rim, opaque
    # out to a third of the radius, fading out at the ring
    t = np.clip(r, 0, 1)
    g = 207 - 147 * np.clip(t / 0.97, 0, 1) ** 1.3
    a = 128 * np.clip(1 - np.clip((t - 0.35) / 0.62, 0, 1) ** 1.6, 0, 1) * (t < 0.98)
    out = np.dstack([np.full_like(t, 255), g, np.zeros_like(t), a]).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    pics = {"hud-mapring-01": ring(), "hud-mapring-alarm-01": alarm()}
    for game in GAMES:
        d = ROOT / "game/assets/enhanced/hud" / game / "level-default-minimap"
        d.mkdir(parents=True, exist_ok=True)
        for name, im in pics.items():
            im.save(d / f"{name}.png", optimize=True)
            print(d / f"{name}.png")
    if a.check:
        # on a sandy minimap-like background, alpha back to the PC's scale
        bg = Image.new("RGBA", (SIZE * 2, SIZE), (196, 168, 112, 255))
        for i, im in enumerate(pics.values()):
            arr = np.array(im)
            arr[..., 3] = np.clip(arr[..., 3].astype(int) * 2, 0, 255)
            bg.alpha_composite(Image.fromarray(arr), (i * SIZE, 0))
        out = ROOT / "local/minimap_frame_check.png"
        bg.convert("RGB").save(out)
        print(out)


if __name__ == "__main__":
    main()
