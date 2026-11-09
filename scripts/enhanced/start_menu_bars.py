"""The start menu's strip dividers and selection bars, drawn here (no game art), into
game/assets/enhanced/wheel/: bar1.png (a liana), bar2.png (dark eco lightning), bar3.png (light eco
lightning), select.png (the jagged gold bar beside the game picked). Tall and thin: the menu
stretches them to the screen's height."""
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

OUT = Path(__file__).resolve().parents[2] / "game/assets/enhanced/wheel"
H = 2048
W = 72


def liana(seed=1):
    # two twisting strands with leaves
    rnd = random.Random(seed)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for ph, col, wid in ((0.0, (52, 92, 30, 255), 10), (math.pi, (78, 128, 42, 255), 8)):
        pts = [(W / 2 + math.sin(y / 60 + ph) * W * 0.22, y) for y in range(0, H, 3)]
        d.line(pts, fill=(28, 46, 16, 255), width=wid + 4, joint="curve")
        d.line(pts, fill=col, width=wid, joint="curve")
    for y in range(14, H, 46):
        x = W / 2 + math.sin(y / 60) * W * 0.22
        side = 1 if (y // 46) % 2 else -1
        s = rnd.randint(11, 15)
        leaf = [(x, y), (x + side * s * 1.4, y - s * 0.7), (x + side * s * 2.1, y + s * 0.2),
                (x + side * s * 1.0, y + s * 0.6)]
        d.polygon(leaf, fill=(rnd.randint(50, 80), rnd.randint(140, 185), rnd.randint(40, 60), 255),
                  outline=(24, 60, 18, 255))
    return im


def bolt(d, rnd, col, width):
    x, y, pts = W / 2, 0, [(W / 2, 0)]
    while y < H:
        y += rnd.randint(18, 40)
        x = min(max(x + rnd.uniform(-W * 0.25, W * 0.25), W * 0.15), W * 0.85)
        pts.append((x, y))
        if rnd.random() < 0.18:  # a short fork
            fx, fy = x, y
            for _ in range(rnd.randint(2, 4)):
                nx = min(max(fx + rnd.uniform(-W * 0.3, W * 0.3), 2), W - 2)
                ny = fy + rnd.randint(12, 28)
                d.line((fx, fy, nx, ny), fill=col, width=max(1, width - 2))
                fx, fy = nx, ny
    d.line(pts, fill=col, width=width, joint="curve")


def lightning(core, glow, seed, dark):
    # forked eco bolts in a glow (dark eco: a black core inside the purple)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    for k in range(3):
        bolt(hd, random.Random(seed * 10 + k), glow + (255,), 12)
    im.alpha_composite(halo.filter(ImageFilter.GaussianBlur(8)))
    d = ImageDraw.Draw(im)
    for k in range(3):
        if dark:
            bolt(d, random.Random(seed * 10 + k), (10, 0, 18, 255), 7)
        bolt(d, random.Random(seed * 10 + k), core + (255,), 3)
    return im


def select_bar():
    w = 36
    im = Image.new("RGBA", (w, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    step = 30
    pts = [(w * (0.25 if i % 2 else 0.75), y) for i, y in enumerate(range(-step, H + step, step))]
    d.line(pts, fill=(90, 50, 0, 255), width=14, joint="curve")
    d.line(pts, fill=(255, 200, 60, 255), width=8, joint="curve")
    return im


if __name__ == "__main__":
    liana().save(OUT / "bar1.png")
    lightning((200, 90, 255), (110, 30, 170), 2, True).save(OUT / "bar2.png")
    lightning((235, 250, 255), (120, 200, 255), 3, False).save(OUT / "bar3.png")
    select_bar().save(OUT / "select.png")
    print("bars ->", OUT)
