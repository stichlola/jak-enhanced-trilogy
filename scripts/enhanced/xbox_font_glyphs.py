"""Jak Enhanced Trilogy: the pad glyphs of the game font, redrawn sharp (Jak 2 and 3).

    python scripts/enhanced/xbox_font_glyphs.py [--check] [--strings]

The big font (gamefont, font.24lo) has the pad's pieces as glyphs: the round button with its shine
and rim, the d-pad's four arms, the shoulder and trigger bodies (four cells each) with their names,
the stick and its arrows, the select pill. The game texts put them together with move codes
(common/util/font/dbs/font_db_jak3.cpp, <PAD_...>). This draws them all again, by hand (lines, arcs
and polygons below: no game art, no font file), four times finer than the game's texture, plus what
an Xbox pad needs and the font has not: the letters A B X Y, the names LB RB LT RT, the View and
Menu signs.

It writes game/assets/enhanced/hud/<game>/gamefont/font.24lo.png, 4x the game's size: see-through
magenta everywhere (the game's own texel there, EnhancedMapTextures.cpp) but the cells drawn here,
so it can be in git.

A glyph's quad is 24 x 29.714 screen units (512 x 416 screen, so a unit is 13/12 wider than tall)
showing a 23 x 31.75 texel window: everything below is drawn in square units (x * 13/12, y), in the
space of the whole button (the pen at 0, 0), and each cell takes its part of it.

--strings prints the Xbox buttons' move codes as they are in common/util/font/font_utils.cpp and in
goal_src/jak{2,3}/pc/progress/progress-generic-pc.gc (they must be changed together with the pens
here).
--check saves contact sheets, before and after, drawn as the game does over its own font
(decompiler_out, not in git): local/pad_glyphs_<game>_{4k,small}.png.
"""

import argparse
import io
import math
import re
import subprocess
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
GAMES = {"jak2": "gamefont", "jak3": "gamefont"}
TEX = "font.24lo"
TEX_W, TEX_H = 256, 1024
SCALE = 4  # texels here per game texel
SS = 6  # drawn this much finer again, then averaged down
MAGENTA = (255, 0, 255, 0)

QUAD_W, QUAD_H = 24.0, 29.714  # a glyph's quad, screen units
ST_W, ST_H = 0.08985 * TEX_W, 0.031007752 * TEX_H  # its window, texels
UX = 13.0 / 12.0  # a screen unit's width over its height (512 x 416 on a 4:3 screen)

# --- the glyph codes ----------------------------------------------------------------------------
DISC, SHINE, RIM = 0x3C, 0x3E, 0x5B  # '<' '>' '[': the round button's body, shine and rim
STICK, SELECT = 0x91, 0x92
NAME_R1, NAME_L1, NAME_R2, NAME_L2 = 0x94, 0x95, 0x96, 0x97
ARROW = {0x98: 0, 0x99: 45, 0xA4: 90, 0xA5: 135, 0x9C: 180, 0x9D: 225, 0x9E: 270, 0x9F: 315}  # tip, clockwise from up
DPAD_D, DPAD_L, DPAD_U, DPAD_R = 0xA0, 0xA1, 0xA2, 0xA3
SH_TL, SH_TR, SH_BL, SH_BR = 0xA6, 0xA7, 0xB0, 0xB1
TR_TL, TR_TR, TR_BL, TR_BR = 0xA8, 0xA9, 0xB2, 0xB3
# cells the game's font leaves empty
LET_A, LET_B, LET_X, LET_Y = 0xAD, 0xAE, 0xAF, 0xB8
SIGN_VIEW, SIGN_MENU = 0xB9, 0xAC

# --- one look for all ---------------------------------------------------------------------------
CAP_MID = 12.9  # the middle of the font's capitals, units down from the pen: every button sits on it
BTN_C = (13.0, CAP_MID)  # the round button's center (square units) and radius
BTN_R = 12.6
LINE = 1.3  # outlines: the rim, the stick's ring, the signs
FACE_H, FACE_S = 14.4, 2.9  # the face letters: height and stroke
NAME_H, NAME_S = 9.6, 1.95  # the shoulder names: the same weight for their size
BODY_CX = 17.5 * UX  # shoulder and trigger: their middle
SHOULDER = (36.4, 18.6, 6.2)  # width, height, corner radius
TRIGGER = (27.0, 26.0, 10.0, 4.0)  # width, height, top and bottom corner radius
TRIGGER_NAME_Y = CAP_MID + 1.6  # (the trigger's top is round: its name sits a little low)
DPAD_ARM = (18.5, 13.2)  # an arm's length (outer end to tip) and width; the tip is a right angle
STICK_C = (11.5 * UX, 14.0)  # in its cell
STICK_R, STICK_CAP = 9.75, 5.2
ARROW_C = (11.5 * UX, 14.0)  # every arrow's middle, in its cell
ARROW_SIZE = (8.0, 6.4)  # base, height

# --- where the cells of a button go: pen offsets (screen units), as in the texts' move codes ------
SHOULDER_PENS = {SH_TL: (-2, -12), SH_TR: (14, -12), SH_BL: (-2, 17), SH_BR: (14, 17)}
TRIGGER_PENS = {TR_TL: (-2, -6), TR_TR: (14, -6), TR_BL: (-2, 16), TR_BR: (14, 16)}
NAME_PENS = {NAME_L1: (4, 3), NAME_R1: (6, 3), NAME_L2: (5, -2), NAME_R2: (5, -2)}
# the d-pad's arm tips, in their cells (screen units): where the game's own art has them, so the
# PlayStation layout and the flags made of these arms hold; whole numbers, so the Xbox cross closes
DPAD_TIP = {DPAD_L: (20, 13), DPAD_R: (1, 13), DPAD_U: (10, 21), DPAD_D: (10, 5)}
DPAD_DIR = {DPAD_L: (1, 0), DPAD_R: (-1, 0), DPAD_U: (0, 1), DPAD_D: (0, -1)}  # the tip points there
DPAD_CENTER = (18, 13)  # the Xbox cross: all four tips there
DPAD_ADVANCE = 38
STICK_PEN = 6  # the stick with arrows all around: its cell this far right
STICK_ADVANCE = 37
ARROW_REACH = {0: (0, -15), 45: (10, -11), 90: (14, 0), 135: (10, 11), 180: (0, 15), 225: (-10, 11),
               270: (-14, 0), 315: (-10, -11)}


# --- drawing: masks in a cell's own grid, shapes given in square units ----------------------------
def floor4(v):
    return math.floor(v * 10000) / 10000  # (the game's tables keep four decimals)


class Cell:
    """A font cell and the part of a button it shows: `pen` is the cell's offset in the button."""

    def __init__(self, code, pen=(0.0, 0.0)):
        i = code - 6
        self.code, self.col, self.row = code, i % 10, i // 10
        tx0 = floor4((1 + 24 * self.col) / TEX_W) * TEX_W
        ty0 = floor4((1 + 32 * self.row) / TEX_H) * TEX_H
        k = SCALE * SS
        # the canvas: the cell's 24 x 32 texels, from 3 fine rows down (the first 3 show in the
        # window of the cell above: they belong to it)
        self.size = (24 * k, 32 * k)
        self.kx = ST_W / QUAD_W * k / UX
        self.ky = ST_H / QUAD_H * k
        self.ox = (tx0 - 24 * self.col) * k - pen[0] * UX * self.kx
        self.oy = (ty0 - 32 * self.row) * k - 3 * SS - pen[1] * self.ky

    def pt(self, p):
        return (p[0] * self.kx + self.ox, p[1] * self.ky + self.oy)

    def blank(self):
        return Image.new("L", self.size, 0)

    def poly(self, pts):
        m = self.blank()
        ImageDraw.Draw(m).polygon([self.pt(p) for p in pts], fill=255)
        return m

    def rect(self, x0, y0, x1, y1):
        return self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])

    def disc(self, c, r):
        m = self.blank()
        (x0, y0), (x1, y1) = self.pt((c[0] - r, c[1] - r)), self.pt((c[0] + r, c[1] + r))
        ImageDraw.Draw(m).ellipse((x0, y0, x1, y1), fill=255)
        return m

    def ring(self, c, r, w):
        return minus(self.disc(c, r), self.disc(c, r - w))

    def rrect(self, x0, y0, x1, y1, r):
        """Rounded rectangle; r is one radius or (top-left, top-right, bottom-right, bottom-left)."""
        rs = (r,) * 4 if isinstance(r, (int, float)) else r
        corners = [(x0 + rs[0], y0 + rs[0], rs[0], 180), (x1 - rs[1], y0 + rs[1], rs[1], 270),
                   (x1 - rs[2], y1 - rs[2], rs[2], 0), (x0 + rs[3], y1 - rs[3], rs[3], 90)]
        pts = []
        for cx, cy, rr, a0 in corners:
            for s in range(25):
                a = math.radians(a0 + 90 * s / 24)
                pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        return self.poly(pts)


def union(*ms):
    out = ms[0]
    for m in ms[1:]:
        out = ImageChops.lighter(out, m)
    return out


def both(a, b):
    return ImageChops.darker(a, b)


def minus(a, b):
    return ImageChops.darker(a, ImageChops.invert(b))


# --- letters: plain bold strokes, flat ends, one weight ---------------------------------------------
LETTER_W = {"A": 0.96, "X": 0.9, "Y": 0.92, "B": 0.7, "R": 0.7, "L": 0.58, "T": 0.72}  # over the height
NAME_GAP = {"LB": 1.25, "RB": 1.25, "LT": 0.45, "RT": 0.9}


def slant_width(s, dx, h):
    """Horizontal width of a stroke s thick that runs dx sideways over a height h."""
    return s * math.hypot(dx, h) / h


def letter(c, ch, cx, cy, h, s):
    """The letter's mask, centered at cx (its own width) and cy."""
    t, b = cy - h / 2, cy + h / 2
    w = LETTER_W[ch] * h
    l = cx - w / 2
    if ch == "A":
        sh = s
        for _ in range(8):  # (the legs' horizontal width depends on their slant, which depends on it)
            ah = 0.55 * sh
            sh = slant_width(s, w / 2 - ah, h)
        ah = 0.55 * sh  # half the flat top
        hull = c.poly([(l, b), (l + w, b), (cx + ah, t), (cx - ah, t)])
        legs = union(c.poly([(l, b), (l + sh, b), (cx - ah + sh, t), (cx - ah, t)]),
                     c.poly([(l + w, b), (l + w - sh, b), (cx + ah - sh, t), (cx + ah, t)]))
        bar_y = b - h * 0.34
        return union(legs, both(hull, c.rect(l, bar_y, l + w, bar_y + s * 0.92)))
    if ch == "X":
        sh = s
        for _ in range(8):
            sh = slant_width(s, w - sh, h)
        return union(c.poly([(l, t), (l + sh, t), (l + w, b), (l + w - sh, b)]),
                     c.poly([(l + w, t), (l + w - sh, t), (l, b), (l + sh, b)]))
    if ch == "Y":
        yj = cy + h * 0.06  # where the arms' outer edges reach the stem
        tan = (w / 2 - s / 2) / (yj - t)
        sh = s * math.hypot(tan, 1.0)
        yc = t + (w / 2 - sh) / tan  # the crotch
        return c.poly([(l, t), (l + sh, t), (cx, yc), (l + w - sh, t), (l + w, t),
                       (cx + s / 2, yj), (cx + s / 2, b), (cx - s / 2, b), (cx - s / 2, yj)])
    if ch == "B":
        ym = cy - h * 0.03  # the middle bar
        up_r, lo_r = l + w * 0.93, l + w
        ru, rl = (ym + s / 2 - t) / 2, (b - ym + s / 2) / 2
        outer = union(c.rrect(l, t, up_r, ym + s / 2, (0, ru, ru, 0)), c.rrect(l, ym - s / 2, lo_r, b, (0, rl, rl, 0)))
        hu, hl = ((ym - s / 2) - (t + s)) / 2, ((b - s) - (ym + s / 2)) / 2
        holes = union(c.rrect(l + s, t + s, up_r - s, ym - s / 2, (0, hu, hu, 0)),
                      c.rrect(l + s, ym + s / 2, lo_r - s, b - s, (0, hl, hl, 0)))
        return minus(outer, holes)
    if ch == "R":
        ym = cy + h * 0.04  # the bowl's lower bar
        r_out = (ym + s / 2 - t) / 2
        r_in = max(0.2, r_out - s)
        bowl = minus(c.rrect(l, t, l + w * 0.96, ym + s / 2, (0, r_out, r_out, 0)),
                     c.rrect(l + s, t + s, l + w * 0.96 - s, ym - s / 2, (0, r_in, r_in, 0)))
        x0 = l + w * 0.42  # the leg, from under the bowl to the foot
        sh = s
        for _ in range(8):
            sh = slant_width(s, l + w - sh - x0, b - ym)
        leg = c.poly([(x0, ym), (x0 + sh, ym), (l + w, b), (l + w - sh, b)])
        return union(bowl, c.rect(l, t, l + s, b), leg)
    if ch == "L":
        return union(c.rect(l, t, l + s, b), c.rect(l, b - s * 0.95, l + w, b))
    if ch == "T":
        return union(c.rect(l, t, l + w, t + s * 0.95), c.rect(cx - s / 2, t, cx + s / 2, b))
    raise ValueError(ch)


def name(c, text, cx, cy):
    """Two letters, centered together."""
    ws = [LETTER_W[ch] * NAME_H for ch in text]
    x = cx - (sum(ws) + NAME_GAP[text]) / 2
    out = c.blank()
    for ch, w in zip(text, ws):
        out = union(out, letter(c, ch, x + w / 2, cy, NAME_H, NAME_S))
        x += w + NAME_GAP[text]
    return out


# --- the buttons ------------------------------------------------------------------------------------
# (A body is made of four cells whose quads overlap on screen. Each cell simply holds all of the
# body its quad covers: where two overlap they agree, and a cell drawn later puts its shadow only
# where it then draws itself. Splitting the outline between them left dark nicks at the seams.)
def shoulder_shape(c):
    w, h, r = SHOULDER
    return c.rrect(BODY_CX - w / 2, CAP_MID - h / 2, BODY_CX + w / 2, CAP_MID + h / 2, r)


def trigger_shape(c):
    w, h, rt, rb = TRIGGER
    return c.rrect(BODY_CX - w / 2, CAP_MID - h / 2, BODY_CX + w / 2, CAP_MID + h / 2, (rt, rt, rb, rb))


def shine(c):
    """The round button's gloss: a soft crescent inside its upper left edge."""
    r = BTN_R - LINE - 0.5
    m = minus(c.disc(BTN_C, r), c.disc((BTN_C[0] + 2.4, BTN_C[1] + 2.4), r + 0.6))
    return both(m.filter(ImageFilter.GaussianBlur(0.45 * c.ky)), c.disc(BTN_C, r))


def dpad_arm(c, code):
    """An arm of the d-pad: a bar with a right-angled tip, at its place in its own cell. The tip goes
    half a unit past where the four meet, so the Xbox cross has no hairlines between its arms."""
    length, width = DPAD_ARM
    (tx, ty), (dx, dy) = DPAD_TIP[code], DPAD_DIR[code]
    tip = (tx * UX + 0.5 * dx, ty + 0.5 * dy)
    hw, back, r = width / 2, length + 0.5, 1.6  # (r: the outer end's corners)
    at = lambda s, q: (tip[0] - dx * s - dy * q, tip[1] - dy * s + dx * q)  # s back from the tip, q across
    return union(c.poly([at(0, 0), at(hw, hw), at(back - r, hw), at(back - r, -hw), at(hw, -hw)]),
                 c.poly([at(back - r, hw - r), at(back, hw - r), at(back, -hw + r), at(back - r, -hw + r)]),
                 c.disc(at(back - r, hw - r), r), c.disc(at(back - r, -hw + r), r))


def arrow(c, deg):
    base, h = ARROW_SIZE
    ca, sa = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    pts = [(0, -h / 2), (-base / 2, h / 2), (base / 2, h / 2)]  # tip up, then turned clockwise
    return c.poly([(ARROW_C[0] + x * ca - y * sa, ARROW_C[1] + x * sa + y * ca) for x, y in pts])


def view_sign(c):
    """Two windows, one over the other."""
    w, h, d, r = 7.6, 6.2, 2.6, 1.2

    def frame(x, y):
        return minus(c.rrect(x - w / 2, y - h / 2, x + w / 2, y + h / 2, r),
                     c.rrect(x - w / 2 + LINE, y - h / 2 + LINE, x + w / 2 - LINE, y + h / 2 - LINE, 0.3))

    fx, fy = BTN_C[0] + d / 2, BTN_C[1] + d / 2
    gap = c.rrect(fx - w / 2 - 0.9, fy - h / 2 - 0.9, fx + w / 2 + 0.9, fy + h / 2 + 0.9, r + 0.9)
    return union(minus(frame(BTN_C[0] - d / 2, BTN_C[1] - d / 2), gap), frame(fx, fy))


def menu_sign(c):
    """Three lines."""
    w, t, step = 10.6, 1.7, 3.7
    return union(*[c.rrect(BTN_C[0] - w / 2, BTN_C[1] + k * step - t / 2, BTN_C[0] + w / 2, BTN_C[1] + k * step + t / 2,
                           t / 2) for k in (-1, 0, 1)])


def cells():
    """Every cell drawn here: (cell, mask, opacity)."""
    out = []

    def add(code, pen, fn, opacity=1.0):
        c = Cell(code, pen)
        out.append((c, fn(c), opacity))

    # the round button: body, rim, shine; then what goes on it
    add(DISC, (0, 0), lambda c: c.disc(BTN_C, BTN_R))
    add(RIM, (0, 0), lambda c: c.ring(BTN_C, BTN_R, LINE))
    add(SHINE, (0, 0), shine, 0.6)
    for code, ch in ((LET_A, "A"), (LET_B, "B"), (LET_X, "X"), (LET_Y, "Y")):
        # (B's heavy side is its stem: a touch right of center)
        add(code, (0, 0), lambda c, ch=ch: letter(c, ch, BTN_C[0] + (0.25 if ch == "B" else 0.0), BTN_C[1], FACE_H, FACE_S))
    add(SIGN_VIEW, (0, 0), view_sign)
    add(SIGN_MENU, (0, 0), menu_sign)

    # shoulder (LB RB): a bar; trigger (LT RT): taller, round at the top
    for code, pen in SHOULDER_PENS.items():
        add(code, pen, shoulder_shape)
    for code, pen in TRIGGER_PENS.items():
        add(code, pen, trigger_shape)
    for code, text, y in ((NAME_L1, "LB", CAP_MID), (NAME_R1, "RB", CAP_MID),
                          (NAME_L2, "LT", TRIGGER_NAME_Y), (NAME_R2, "RT", TRIGGER_NAME_Y)):
        add(code, NAME_PENS[code], lambda c, text=text, y=y: name(c, text, BODY_CX, y))

    for code in DPAD_TIP:
        add(code, (0, 0), lambda c, code=code: dpad_arm(c, code))

    # the stick from above: a ring and its cap; the arrows around it
    add(STICK, (0, 0), lambda c: union(c.ring(STICK_C, STICK_R, LINE + 0.2), c.disc(STICK_C, STICK_CAP)))
    for code, deg in ARROW.items():
        add(code, (0, 0), lambda c, deg=deg: arrow(c, deg))
    add(SELECT, (0, 0), lambda c: c.rrect(0.8, CAP_MID - 5.6, 23.2, CAP_MID + 5.6, 5.6))
    return out


def draw():
    """The texture: (image, the codes of the cells drawn)."""
    im = Image.new("RGBA", (TEX_W * SCALE, TEX_H * SCALE), MAGENTA)
    drawn = cells()
    clear = Image.new("RGBA", (24 * SCALE, 3), (255, 255, 255, 0))
    for c, _, _ in drawn:  # (a cell's first rows show under the cell above it: emptied)
        im.paste(clear, (c.col * 24 * SCALE, c.row * 32 * SCALE))
    for c, mask, opacity in drawn:
        # (opaque is 128 in the PS2's alpha)
        a = mask.resize((24 * SCALE, 32 * SCALE), Image.BOX).point(lambda v: int(v * opacity * 128 / 255 + 0.5))
        white = Image.new("L", a.size, 255)
        im.paste(Image.merge("RGBA", (white, white, white, a)), (c.col * 24 * SCALE, c.row * 32 * SCALE + 3))
    return im, [c.code for c, _, _ in drawn]


# --- the texts' move codes ----------------------------------------------------------------------------
def esc(code, style):
    return {"c": f"\\x{code:02x}", "goal": f"\\c{code:02x}", "raw": chr(code)}[style]


def mv(dx, dy):
    return (f"~{dx:+d}H" if dx else "") + (f"~{dy:+d}V" if dy else "")


def xbox_strings(style="raw"):
    """The Xbox buttons, in the game's text encoding."""
    e = lambda code: esc(code, style)
    out = {}
    # (the PlayStation button's own layout, with the letter's cell and color; the letter after the
    # shine, so the shine's shadow is under it)
    for label, code, color in (("A", LET_A, 26), ("B", LET_B, 25), ("X", LET_X, 27), ("Y", LET_Y, 5)):
        out[label] = f"~Y~22L<~Z~Y~1L>~Z~Y~{color}L{e(code)}~Z~Y~23L[~Z~+26H"
    for label, code in (("VIEW", SIGN_VIEW), ("MENU", SIGN_MENU)):
        out[label] = f"~Y~22L<~Z~Y~1L{e(code)}~Z~Y~23L[~Z~+26H"
    # the d-pad: the four arms closed into a cross ({0} {1} {2} {3}: the colors of left, up, down, right)
    pens = {code: (DPAD_CENTER[0] - DPAD_TIP[code][0], DPAD_CENTER[1] - DPAD_TIP[code][1]) for code in DPAD_TIP}
    out["DPAD"] = "~Y" + "~Z".join(
        f"~{{{k}}}L{mv(*pens[code])}{e(code)}" for k, code in enumerate((DPAD_L, DPAD_U, DPAD_D, DPAD_R))
    ) + f"~Z~+{DPAD_ADVANCE}H"
    # the stick: arrows pointing away from it, all around or on one axis
    by_deg = {deg: code for code, deg in ARROW.items()}
    arrows = lambda degs: "".join(f"~Z~6L{mv(*ARROW_REACH[d])}{e(by_deg[d])}" for d in degs)
    out["ANALOG_ANY"] = (f"~+{STICK_PEN}H~Y~1L{e(STICK)}" + arrows((270, 90, 0, 180, 315, 45, 225, 135))
                         + f"~Z~+{STICK_ADVANCE - STICK_PEN}H")
    out["ANALOG_LEFT_RIGHT"] = f"~+{STICK_PEN}H~Y~1L{e(STICK)}" + arrows((270, 90)) + f"~Z~+{STICK_ADVANCE - STICK_PEN}H"
    out["ANALOG_UP_DOWN"] = f"~Y~1L{e(STICK)}" + arrows((0, 180)) + "~Z~+26H"
    return out


def dpad(strings, left=22, up=22, down=22, right=22):
    return strings["DPAD"].format(left, up, down, right)


# --- check: the glyphs as the game draws them -------------------------------------------------------------
OLD_REV = "0b7de56de"  # the glyphs before these (RB LB RT LT only, at the game's size)


def old_face(letter_code):
    """The Xbox button as it was: the body drawn nine times to grow it, the font's own letter on it."""
    body = ""
    for ox, oy in ((0, 0), (3, 0), (-3, 0), (0, 3), (0, -3), (2, 2), (2, -2), (-2, 2), (-2, -2)):
        body += "~Y" + mv(ox, oy) + "~22L<~Z"
    return f"~+3H{body}~Y{letter_code}~Z~Y~1L>~Z~Y~23L[~Z~+29H"


PS = {  # the game's own layouts (font_db_jak3.cpp)
    "L1": "~Y~22L~-2H~-12V\xa6\xa7~Z~22L~-2H~+17V\xb0\xb1~Z~1L~+4H~+3V\x95~Z~+38H",
    "R1": "~Y~22L~-2H~-12V\xa6\xa7~Z~22L~-2H~+17V\xb0\xb1~Z~1L~+6H~+3V\x94~Z~+38H",
    "L2": "~Y~22L~-2H~-6V\xa8\xa9~Z~22L~-2H~+16V\xb2\xb3~Z~1L~+5H~-2V\x97~Z~+38H",
    "R2": "~Y~22L~-2H~-6V\xa8\xa9~Z~22L~-2H~+16V\xb2\xb3~Z~1L~+5H~-2V\x96~Z~+38H",
    "DPAD_UP": "~Y~22L\xa1~Z~3L~+17H~-13V\xa2~Z~22L~+17H~+14V\xa0~Z~22L~+32H\xa3~Z~+56H",
    "DPAD_ANY": "~Y~22L\xa1~Z~22L~+17H~-13V\xa2~Z~22L~+17H~+14V\xa0~Z~22L~+32H\xa3~Z~+56H",
    "ANALOG_ANY": "~1L~+8H~Y\x91~Z~6L~-16H\x9e~Z~+16h~6L\xa4~Z~6L~-15V\x9c~Z~+13V~6L\x98~Z~-10H~+9V~6L\x9f~Z~+10H~+9V~6L"
                  "\x99~Z~-10H~-11V~6L\x9d~Z~+10H~-11V~6L\xa5~Z~+32H",
    "ANALOG_LEFT_RIGHT": "~Y~1L~+8H\x91~Z~6L~-8H\x9e~Z~+24H~6L\xa4~Z~+40H",
    "ANALOG_UP_DOWN": "~Y~1L\x91~Z~6L~-15V\x9c~Z~+13V~6L\x98~Z~+26H",
    "X": "~Y~22L<~Z~Y~27L*~Z~Y~1L>~Z~Y~23L[~Z~+26H",
    "TRIANGLE": "~Y~22L<~Z~Y~26L;~Z~Y~1L>~Z~Y~23L[~Z~+26H",
    "CIRCLE": "~Y~22L<~Z~Y~25L@~Z~Y~1L>~Z~Y~23L[~Z~+26H",
    "SQUARE": "~Y~22L<~Z~Y~24L#~Z~Y~1L>~Z~Y~23L[~Z~+26H",
    "DONE_TODO": "~Y~6L<~Z~Y~1L>~Z~Y~23L[~Z~+26H~Y~3L<~Z~Y~1L>~Z~Y~23L[~Z~+26H",
    "BRAZIL": "~Y~26L\x85~]~-1H~Y~26L\x85~Z~-10H~-3V~5L\xa0~Z~-10H~+6V~5L\xa2~Z~-5H~+1V~5L\xa1~Z~-14H~+1V~5L\xa3~Z"
              "~-11H~+1V~7L\x8a~Z~+26H",
}


def samples():
    """(label, before, after): what the game shows for each button, as it was and as it is now."""
    x = xbox_strings()
    faces = PS["X"] + PS["TRIANGLE"] + PS["CIRCLE"] + PS["SQUARE"]
    return [
        ("A", old_face("~26L~+1HA"), x["A"]), ("B", old_face("~25L~+3HB"), x["B"]),
        ("X", old_face("~27L~+1HX"), x["X"]), ("Y", old_face("~5L~+2HY"), x["Y"]),
        ("LB", PS["L1"], PS["L1"]), ("RB", PS["R1"], PS["R1"]), ("LT", PS["L2"], PS["L2"]), ("RT", PS["R2"], PS["R2"]),
        ("d-pad up", PS["DPAD_UP"], dpad(x, up=3)), ("d-pad", PS["DPAD_ANY"], dpad(x)),
        ("d-pad down (keys menu)", "~Y~0L\xa1~Z~0L~+17H~-13V\xa2~Z~3L~+17H~+14V\xa0~Z~0L~+32H\xa3~Z~+56H", dpad(x, 0, 0, 3, 0)),
        ("stick", PS["ANALOG_ANY"], x["ANALOG_ANY"]),
        ("stick left right", PS["ANALOG_LEFT_RIGHT"], x["ANALOG_LEFT_RIGHT"]),
        ("stick up down", PS["ANALOG_UP_DOWN"], x["ANALOG_UP_DOWN"]),
        ("stick up (keys menu)", "~Y~1L\x91~Z~-17V~6L\x98~Z~+26H", "~Y~1L\x91~Z~-15V~6L\x98~Z~+26H"),
        ("L3 / LS", "\x91 L3", "\x91 LS"),
        ("View", "\x92 VIEW", x["VIEW"] + " VIEW"), ("Menu", "\x92 MENU", x["MENU"] + " MENU"),
        ("done, to do", PS["DONE_TODO"], PS["DONE_TODO"]),
        ("PlayStation buttons (Light)", faces, faces),
        ("PlayStation d-pad, stick (Light)", PS["DPAD_UP"] + PS["ANALOG_ANY"], PS["DPAD_UP"] + PS["ANALOG_ANY"]),
        ("Brazil's flag (made of d-pad arms)", PS["BRAZIL"], PS["BRAZIL"]),
    ]


class Sim:
    """Draws text in the game's encoding as draw-string does (kerning on, shadow): quads of 24 x 29.714
    units, the move codes, the color table; the texture filtered as on the GPU."""

    def __init__(self, game, overlay):
        import numpy as np

        self.np = np
        src = (ROOT / "goal_src" / game / "engine/data/font-data.gc").read_text()
        src = src[src.index("*font24-table*"):]
        self.table = [(float(x) * TEX_W, float(y) * TEX_H, float(w))
                      for x, y, w in re.findall(r":x ([\d.]+) :y ([\d.]+) :z [\d.]+ :w ([\d.]+)", src)]
        src = (ROOT / "goal_src" / game / "engine/gfx/font-h.gc").read_text()
        src = src[src.index(":color-table"):]
        self.colors = []
        for ent in re.findall(r"\(new 'static 'rgba((?: :[rgba] #x[0-9a-f]+)*)\)", src)[::4]:
            d = dict(re.findall(r":([rgba]) #x([0-9a-f]+)", ent))
            self.colors.append(tuple(int(d.get(k, "0"), 16) for k in "rgba"))
        base = None
        # (a git worktree has no decompiler_out of its own: the main tree's)
        common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True).stdout.strip()
        for root in [ROOT] + ([Path(common).parent] if common else []):
            for folder in (f"{game}_pal", game):
                p = root / "decompiler_out" / folder / "textures" / GAMES[game] / f"{TEX}.png"
                if base is None and p.exists():
                    base = Image.open(p).convert("RGBA")
        if base is None:
            raise FileNotFoundError(f"decompiler_out/{game}*/textures/{GAMES[game]}/{TEX}.png")
        b = np.asarray(base).astype(np.float32)[..., 3]
        o = np.asarray(overlay.convert("RGBA")).astype(np.float32)
        s = self.s = o.shape[1] // TEX_W
        # as EnhancedMapTextures.cpp: magenta = the game's texel, enlarged bilinearly
        fx = np.clip((np.arange(o.shape[1]) + 0.5) / s - 0.5, 0, TEX_W - 1)
        fy = np.clip((np.arange(o.shape[0]) + 0.5) / s - 0.5, 0, TEX_H - 1)
        x0, y0 = fx.astype(int), fy.astype(int)
        x1, y1 = np.minimum(x0 + 1, TEX_W - 1), np.minimum(y0 + 1, TEX_H - 1)
        tx, ty = (fx - x0)[None, :], (fy - y0)[:, None]
        up = (b[y0][:, x0] * (1 - tx) + b[y0][:, x1] * tx) * (1 - ty) + (b[y1][:, x0] * (1 - tx) + b[y1][:, x1] * tx) * ty
        keep = (o[..., 0] == 255) & (o[..., 1] == 0) & (o[..., 2] == 255) & (o[..., 3] == 0)
        self.tex = Image.fromarray(np.clip(np.where(keep, up, o[..., 3]) / 128.0, 0, 1).astype(np.float32), "F")

    def text(self, canvas, text, x, y, height, scale=1.0, color=1):
        """canvas: RGB byte array; x, y: pixels; height: the screen's (416 units). Returns the pen's x."""
        uy = height / 416.0
        ux = uy * UX
        px, py = x / ux, y / uy
        saved = (px, py, color)  # (~Y keeps the pen and the color, ~Z brings both back)
        b = text.encode("latin-1")
        i = 0
        while i < len(b):
            m = re.match(rb"~([+-]?\d*)([A-Za-z\[\]])", b[i:]) if b[i] == ord("~") else None
            if m:
                num, cmd = m.group(1), m.group(2).upper()
                i += len(m.group(0))
                if cmd == b"Y":
                    saved = (px, py, color)
                elif cmd == b"Z":
                    px, py, color = saved
                elif cmd == b"L":
                    color = int(num)
                elif cmd == b"H":
                    px += int(num) * scale
                elif cmd == b"V":
                    py += int(num) * scale
                continue
            tx, ty, adv = self.table[b[i] - 6]
            w, h = max(1, round(QUAD_W * scale * ux)), max(1, round(QUAD_H * scale * uy))
            s = self.s
            a = self.np.asarray(self.tex.resize((w, h), Image.BILINEAR, box=(tx * s, ty * s, (tx + ST_W) * s, (ty + ST_H) * s)))
            r, g, bl, al = self.colors[color]
            a = self.np.clip(a * (al / 128.0), 0, 1)
            x0, y0 = round(px * ux), round(py * uy)
            self._blend(canvas, a, (0, 0, 0), x0 + round(2 * ux), y0 + round(uy))  # the shadow
            self._blend(canvas, a, (min(r / 128, 1), min(g / 128, 1), min(bl / 128, 1)), x0, y0)
            px += adv * scale
            i += 1
        return px * ux

    def _blend(self, canvas, a, col, x0, y0):
        H, W = canvas.shape[:2]
        h, w = a.shape
        cx0, cy0, cx1, cy1 = max(x0, 0), max(y0, 0), min(x0 + w, W), min(y0 + h, H)
        if cx1 > cx0 and cy1 > cy0:
            aa = a[cy0 - y0:cy1 - y0, cx0 - x0:cx1 - x0, None]
            mixed = canvas[cy0:cy1, cx0:cx1] * (1 - aa) + self.np.array(col, self.np.float32) * 255 * aa
            canvas[cy0:cy1, cx0:cx1] = (mixed + 0.5).astype(self.np.uint8)


def sheet(before, after, height, scale, zoom, path):
    """Rows of buttons, before | after, each over a dark and a light ground, at `height` pixels of
    screen and font scale `scale`; `zoom` enlarges the result (nearest), to judge small sizes."""
    import numpy as np

    rows = samples()
    u = height / 416.0 * scale
    row_h, col_w = round(50 * u), round(215 * u)
    canvas = np.zeros((len(rows) * row_h, 4 * col_w, 3), np.uint8)
    for k, ground in enumerate(((18, 20, 26), (107, 115, 102)) * 2):
        canvas[:, k * col_w:(k + 1) * col_w] = ground
    for r, (_, old, new) in enumerate(rows):
        for k in range(4):
            sim, s = (before, old) if k < 2 else (after, new)
            x, y = k * col_w + round(6 * u), r * row_h + round(11 * u)
            x = sim.text(canvas, "ON ", x, y, height, scale)
            x = sim.text(canvas, s, x, y, height, scale)
            sim.text(canvas, " GO", x, y, height, scale)
    canvas[:, 2 * col_w - 1:2 * col_w + 1] = (255, 204, 51)  # before | after
    im = Image.fromarray(canvas)
    if zoom != 1:
        im = im.resize((im.width * zoom, im.height * zoom), Image.NEAREST)
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path)
    print(path, im.size, "- left: before, right: after; rows:", ", ".join(label for label, _, _ in rows))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--strings", action="store_true")
    args = ap.parse_args()
    if args.strings:
        for style in ("c", "goal"):
            print(f"--- {style}")
            for label, s in xbox_strings(style).items():
                print(f"{label}: {s}")
        return
    im, codes = draw()
    for game, tpage in GAMES.items():
        out = ROOT / "game/assets/enhanced/hud" / game / tpage / f"{TEX}.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        im.save(out, optimize=True)
        print(out, im.size, f"{len(codes)} cells")
    if args.check:
        old = subprocess.run(["git", "-C", str(ROOT), "show", f"{OLD_REV}:game/assets/enhanced/hud/jak3/gamefont/{TEX}.png"],
                             capture_output=True, check=True).stdout
        old = Image.open(io.BytesIO(old))
        for game in GAMES:
            try:
                before, after = Sim(game, old), Sim(game, im)
            except FileNotFoundError as e:
                print(f"{game}: no check ({e})")
                continue
            sheet(before, after, 2160, 1.0, 1, ROOT / f"local/pad_glyphs_{game}_4k.png")
            sheet(before, after, 720, 0.6, 4, ROOT / f"local/pad_glyphs_{game}_small.png")


if __name__ == "__main__":
    main()
