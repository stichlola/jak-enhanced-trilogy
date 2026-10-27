"""Jak Enhanced Trilogy: the font's shoulder-button names as an Xbox pad's (R1 L1 R2 L2 -> RB LB RT LT).

    python scripts/enhanced/xbox_font_glyphs.py [--check]

The names are four small glyphs of the game font (\\x94..\\x97, <PAD_PART_R1_NAME> ...), drawn inside
the shoulder and trigger outlines. This writes game/assets/enhanced/hud/<game>/gamefont/font.24lo.png
(the small font has none): see-through magenta everywhere (the game's own texel there,
EnhancedMapTextures.cpp) but the four glyph cells, redrawn here with a bold sans. Nothing of the
game's art goes in the file, so it can be in git. --check also saves a preview over the game's
own font (decompiler_out, not in git) to local/xbox_font_check.png.
"""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
FONT = "C:/Windows/Fonts/arialbd.ttf"
NAMES = ["RB", "LB", "RT", "LT"]  # in the order of the glyphs: R1 L1 R2 L2

# per texture: its size and the four glyph boxes (x0, y0, x1, y1), as in the game's own font
TEXTURES = {
    "font.24lo": ((256, 1024), [(52, 457, 69, 471), (73, 457, 94, 471), (98, 457, 118, 471), (121, 457, 142, 471)]),
}
# (Jak 1's font has no such glyphs: its texts say L1 and R1 in letters)
GAMES = {"jak2": "gamefont", "jak3": "gamefont"}
MAGENTA = (255, 0, 255, 0)


def draw(size, boxes):
    im = Image.new("RGBA", size, MAGENTA)
    d = ImageDraw.Draw(im)
    # one size for the four (the biggest that fits every cell)
    px = 32
    for name, (x0, y0, x1, y1) in zip(NAMES, boxes):
        while px > 6:
            l, t, r, b = d.textbbox((0, 0), name, font=ImageFont.truetype(FONT, px))
            if r - l <= x1 - x0 + 2 and b - t <= y1 - y0 + 1:
                break
            px -= 1
    for name, (x0, y0, x1, y1) in zip(NAMES, boxes):
        w, h = x1 - x0 + 1, y1 - y0 + 1
        # the cell cleared (the old name gone), then the new one centered
        d.rectangle((x0, y0, x1, y1), fill=(255, 255, 255, 0))
        # drawn 4x and brought down: smooth edges in the PS2's alpha (128 opaque)
        S = 4
        cell = Image.new("L", (w * S, h * S), 0)
        fs = ImageFont.truetype(FONT, px * S)
        cd = ImageDraw.Draw(cell)
        l, t, r, b = cd.textbbox((0, 0), name, font=fs)
        cd.text(((w * S - (r - l)) / 2 - l, (h * S - (b - t)) / 2 - t), name, font=fs, fill=255)
        a = cell.resize((w, h), Image.LANCZOS).point(lambda v: v * 128 // 255)
        glyph = Image.merge("RGBA", (Image.new("L", (w, h), 255),) * 3 + (a,))
        im.paste(glyph, (x0, y0))
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    for game, tpage in GAMES.items():
        for tex, (size, boxes) in TEXTURES.items():
            out = ROOT / "game/assets/enhanced/hud" / game / tpage / f"{tex}.png"
            out.parent.mkdir(parents=True, exist_ok=True)
            im = draw(size, boxes)
            im.save(out)
            print(out)
            if args.check:
                src = ROOT / "decompiler_out" / f"{game}_pal" / "textures" / tpage / f"{tex}.png"
                base = Image.open(src).convert("RGBA")
                keep = [p == MAGENTA for p in im.get_flattened_data()]
                mixed = Image.new("RGBA", size)
                mixed.putdata([b if k else n for b, n, k in zip(base.get_flattened_data(), im.get_flattened_data(), keep)])
                a = mixed.getchannel("A").point(lambda v: min(255, v * 2))
                x0 = min(b[0] for b in boxes) - 4
                y0 = min(b[1] for b in boxes) - 4
                a.crop((x0, y0, x0 + 100, y0 + 24)).resize((600, 144), Image.NEAREST).save(
                    ROOT / "local/xbox_font_check.png")


if __name__ == "__main__":
    main()
