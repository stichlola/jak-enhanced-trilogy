"""The start menu's strip dividers, drawn here (no game art), into game/assets/enhanced/wheel/:
bar1.png (a liana: the twisted strands, bark and tendrils), bar1_leaves.png (its leaves and flowers,
a layer of their own so the menu can pop them open one band at a time), bar2.png (dark eco
lightning) and bar3.png (light eco lightning), the two as sprite sheets of FRAMES bolts side by side
that the menu cycles to make them crackle. Tall and thin: the menu stretches them to the screen's
height. Everything is drawn at twice the size and shrunk, for smooth edges."""
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

OUT = Path(__file__).resolve().parents[2] / "game/assets/enhanced/wheel"
H = 2048
W = 96
FRAMES = 8  # the lightning sheets' frames (EnhancedPostMenu.cpp: kEcoFrames)
SS = 2  # supersampling


def shrink(im):
    return im.resize((im.width // SS, im.height // SS), Image.LANCZOS)


def strand_x(y, phase, w):
    # the two strands twist around the center: a braid
    return w / 2 + math.sin(y / (60 * SS) + phase) * w * 0.27


def liana():
    """The strands: each a dark rim, bark colors, grooves and a highlight; small curling tendrils."""
    w, h = W * SS, H * SS
    rnd = random.Random(1)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # the back strand first (thinner, darker), the front one over it
    for ph, dark, mid, light, wid in ((math.pi, (34, 52, 20), (70, 98, 40), (110, 142, 62), 11 * SS),
                                      (0.0, (40, 58, 22), (86, 118, 48), (132, 168, 76), 15 * SS)):
        pts = [(strand_x(y, ph, w), y) for y in range(0, h + 1, 3)]
        d.line(pts, fill=(18, 30, 10, 255), width=wid + 4 * SS, joint="curve")
        d.line(pts, fill=dark, width=wid, joint="curve")
        # the bark: the lit half, a highlight ridge, dark grooves that twist along the strand
        d.line([(x - wid * 0.14, y) for x, y in pts], fill=mid, width=int(wid * 0.62), joint="curve")
        d.line([(x - wid * 0.26, y) for x, y in pts], fill=light, width=int(wid * 0.22), joint="curve")
        for g in range(3):
            goff = (g - 1) * wid * 0.28
            gpts = [(x + goff + math.sin(y / (37 * SS) + g) * wid * 0.12, y) for x, y in pts]
            d.line(gpts, fill=(30, 46, 16, 160), width=max(1, SS), joint="curve")
        # knots: a few darker rings where a leaf stalk left
        for y in range(rnd.randint(40, 120) * SS, h, rnd.randint(260, 420) * SS):
            x = strand_x(y, ph, w)
            d.ellipse((x - wid * 0.55, y - 3 * SS, x + wid * 0.55, y + 3 * SS), fill=(46, 66, 26, 255),
                      outline=(20, 32, 10, 255))
    # tendrils: a thin spiral unrolling off a strand every so often
    for y in range(60 * SS, h, 150 * SS):
        side = 1 if (y // (150 * SS)) % 2 else -1
        x0 = strand_x(y, 0.0, w) + side * 4 * SS
        r = rnd.uniform(5, 9) * SS
        pts = []
        for i in range(60):
            t = i / 59
            a = t * math.pi * 3.2
            rr = r * (0.25 + t * 0.75)
            pts.append((x0 + side * (t * r * 2.6 + math.cos(a) * rr), y + rr * math.sin(a) + t * r * 1.2))
        d.line(pts, fill=(22, 36, 12, 255), width=3 * SS, joint="curve")
        d.line(pts, fill=(96, 136, 54, 255), width=int(1.4 * SS), joint="curve")
    return shrink(im)


def leaf(d, x, y, side, s, rnd, ang):
    """A pointed leaf from (x, y) leaning `ang` radians off horizontal, with its veins."""
    ca, sa = math.cos(ang), math.sin(ang)

    def p(u, v):  # leaf space: u along the stalk, v across
        return (x + side * (u * ca - v * sa), y + (u * sa + v * ca))

    n = 14
    outline = [p(0, 0)]
    for i in range(1, n):
        t = i / n
        outline.append(p(t * s * 2.2, -math.sin(t * math.pi) ** 0.8 * s * 0.75))
    outline.append(p(s * 2.2, 0))
    for i in range(n - 1, 0, -1):
        t = i / n
        outline.append(p(t * s * 2.2, math.sin(t * math.pi) ** 0.8 * s * 0.75))
    g = rnd.randint(130, 180)
    base = (rnd.randint(40, 70), g, rnd.randint(36, 60), 255)
    dark = (base[0] - 20, g - 45, base[2] - 14, 255)
    d.polygon(outline, fill=base, outline=(22, 54, 16, 255))
    # the far half darker (the leaf bends away from the light)
    half = [p(0, 0)]
    for i in range(1, n):
        t = i / n
        half.append(p(t * s * 2.2, math.sin(t * math.pi) ** 0.8 * s * 0.75))
    half.append(p(s * 2.2, 0))
    d.polygon(half, fill=dark)
    # the veins: the midrib and side veins curving to the tip
    d.line([p(0, 0), p(s * 2.2, 0)], fill=(200, 230, 150, 210), width=max(1, SS))
    for i in range(1, 5):
        t = i / 5.5
        for sgn in (-1, 1):
            d.line([p(t * s * 2.2, 0), p(t * s * 2.2 + s * 0.6, sgn * math.sin(t * math.pi) ** 0.8 * s * 0.55)],
                   fill=(180, 214, 130, 150), width=max(1, SS // 2 + 1))
    # the highlight along the lit half
    d.line([p(s * 0.4, -s * 0.25), p(s * 1.6, -s * 0.2)], fill=(190, 225, 140, 90), width=int(1.5 * SS))


def flower(d, x, y, rnd):
    r = rnd.uniform(5, 7) * SS
    for i in range(6):
        a = i / 6 * math.tau
        px, py = x + math.cos(a) * r * 0.9, y + math.sin(a) * r * 0.9
        d.ellipse((px - r * 0.62, py - r * 0.62, px + r * 0.62, py + r * 0.62),
                  fill=(238, 168, 215, 255), outline=(170, 90, 150, 255))
    d.ellipse((x - r * 0.42, y - r * 0.42, x + r * 0.42, y + r * 0.42), fill=(255, 220, 90, 255),
              outline=(190, 140, 40, 255))


def liana_leaves():
    """The leaves and flowers only (over the strands), various sizes and angles."""
    w, h = W * SS, H * SS
    rnd = random.Random(7)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    y = 20 * SS
    k = 0
    while y < h - 20 * SS:
        side = 1 if k % 2 else -1
        ph = 0.0 if k % 3 else math.pi
        x = strand_x(y, ph, w) + side * 3 * SS
        s = rnd.uniform(10, 17) * SS
        ang = rnd.uniform(-0.55, 0.35)
        # a short stalk, then the leaf
        d.line([(x, y), (x + side * s * 0.4, y + s * 0.15)], fill=(50, 84, 30, 255), width=2 * SS)
        leaf(d, x + side * s * 0.35, y + s * 0.12, side, s, rnd, ang)
        if rnd.random() < 0.3:  # a second, smaller leaf a bit lower on the other side
            s2 = s * rnd.uniform(0.5, 0.75)
            leaf(d, strand_x(y + 14 * SS, ph, w) - side * 2 * SS, y + 14 * SS, -side, s2, rnd,
                 rnd.uniform(-0.2, 0.6))
        if rnd.random() < 0.12:
            flower(d, x + side * s * 0.9, y - s * 0.5, rnd)
        y += rnd.randint(36, 54) * SS
        k += 1
    return shrink(im)


def bolt_path(rnd, x0, y0, x1, y1, jag, depth=5):
    """Midpoint displacement between two points: a jagged path of 2**depth segments."""
    pts = [(x0, y0), (x1, y1)]
    for _ in range(depth):
        out = [pts[0]]
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            mx, my = (ax + bx) / 2, (ay + by) / 2
            # push sideways (mostly in x: the bolt runs down the strip)
            out.append((mx + rnd.uniform(-jag, jag), my + rnd.uniform(-jag, jag) * 0.25))
            out.append((bx, by))
        pts = out
        jag *= 0.55
    return pts


def draw_bolt(layers, rnd, pts, w, level, dark):
    """One bolt on the halo / glow / core layers, thinner at deeper branch levels."""
    halo, glow, core, rim = layers
    width = max(1, int((7 - 2 * level) * SS))
    halo.line(pts, fill=(255, 255, 255, 255), width=width * 3, joint="curve")
    glow.line(pts, fill=(255, 255, 255, 255), width=width + 2 * SS, joint="curve")
    if dark:
        rim.line(pts, fill=(255, 255, 255, 255), width=width + 2 * SS, joint="curve")
    core.line(pts, fill=(255, 255, 255, 255), width=max(1, width - (SS if dark else 0)), joint="curve")
    # branches: forks off the path, shorter and thinner
    if level < 2:
        for i in range(2, len(pts) - 2):
            if rnd.random() < 0.10 / (level + 1):
                x, y = pts[i]
                side = rnd.choice((-1, 1))
                ex = min(max(x + side * rnd.uniform(w * 0.15, w * 0.4), 2), w - 2)
                ey = y + rnd.uniform(w * 0.3, w * 0.9)
                draw_bolt(layers, rnd, bolt_path(rnd, x, y, ex, ey, w * 0.08, 3), w, level + 1, dark)


def tint(layer, rgb):
    """A white-on-clear layer recolored."""
    r, g, b = rgb
    solid = Image.new("RGBA", layer.size, (r, g, b, 255))
    solid.putalpha(layer.getchannel("A"))
    return solid


def lightning_frame(rnd, core, glow, halo_c, dark):
    w, h = W * SS, H * SS
    layers = [Image.new("RGBA", (w, h), (0, 0, 0, 0)) for _ in range(4)]
    draws = [ImageDraw.Draw(l) for l in layers]
    # one main bolt the whole height, a second one over part of it
    main = bolt_path(rnd, w * rnd.uniform(0.3, 0.7), -10, w * rnd.uniform(0.3, 0.7), h + 10, w * 0.6, 7)
    main = [(min(max(x, w * 0.08), w * 0.92), y) for x, y in main]
    draw_bolt(draws, rnd, main, w, 0, dark)
    y0 = rnd.uniform(0, h * 0.5)
    second = bolt_path(rnd, w * rnd.uniform(0.3, 0.7), y0, w * rnd.uniform(0.3, 0.7),
                       y0 + rnd.uniform(h * 0.2, h * 0.45), w * 0.4, 6)
    second = [(min(max(x, w * 0.1), w * 0.9), y) for x, y in second]
    draw_bolt(draws, rnd, second, w, 1, dark)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    halo = tint(layers[0].filter(ImageFilter.GaussianBlur(9 * SS)), halo_c)
    halo.putalpha(halo.getchannel("A").point(lambda a: min(255, int(a * (0.7 if dark else 0.8)))))
    im.alpha_composite(halo)
    im.alpha_composite(tint(layers[1].filter(ImageFilter.GaussianBlur(2.2 * SS)), glow))
    if dark:
        im.alpha_composite(tint(layers[3], glow))
    im.alpha_composite(tint(layers[2].filter(ImageFilter.GaussianBlur(0.5 * SS)), core))
    # sparks: tiny glowing dots thrown off the bolt
    sp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sp)
    for _ in range(26):
        x, y = rnd.choice(main)
        x += rnd.uniform(-w * 0.25, w * 0.25)
        y += rnd.uniform(-40, 40) * SS
        r = rnd.uniform(0.8, 2.2) * SS
        sd.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, 255))
    sp = sp.filter(ImageFilter.GaussianBlur(0.8 * SS))
    im.alpha_composite(tint(sp, glow if dark else core))
    return shrink(im)


def lightning(core, glow, halo_c, seed, dark):
    sheet = Image.new("RGBA", (W * FRAMES, H), (0, 0, 0, 0))
    for k in range(FRAMES):
        sheet.paste(lightning_frame(random.Random(seed * 100 + k), core, glow, halo_c, dark), (W * k, 0))
    return sheet


if __name__ == "__main__":
    liana().save(OUT / "bar1.png")
    liana_leaves().save(OUT / "bar1_leaves.png")
    # dark eco: a black core inside violet, a purple haze
    lightning((8, 0, 16), (205, 110, 255), (120, 30, 190), 2, True).save(OUT / "bar2.png")
    # light eco: a white core, cyan-blue glow
    lightning((245, 252, 255), (150, 225, 255), (70, 150, 255), 3, False).save(OUT / "bar3.png")
    print("bars ->", OUT)
