"""The minimap's icons redrawn in high definition with Tripo (image to image), Jak II and 3.

  python scripts/enhanced/minimap_icons.py ref            # the reference sheets (free)
  python scripts/enhanced/minimap_icons.py image <0-3>    # one sheet redrawn by Tripo (~5 credits)
  python scripts/enhanced/minimap_icons.py atlas          # cut the sheets, build the 8x atlases

The game's icon atlas (mini-map-icons, 128 x 128: a grid of 20 x 20 cells) comes from the
decompiler's texture dump; its 47 picture icons go to Tripo in four 4 x 3 sheets, each icon 8x on a
light gray background, with a description of every icon. The redrawn icons are cut back out of the
answer (the background flooded away from the edges up to the icons' dark outlines) and put in the
same cells of a 1024 x 1024 atlas; the plain dots are drawn here. Everything stays in
local/minimap_icons; the finished atlases (new drawings) go in the repo where the game loads them,
game/assets/enhanced/hud/<game>/<tpage>/mini-map-icons.png (EnhancedMapTextures.cpp).
The Tripo key comes from TRIPO_API_KEY and is never printed.
"""
import json
import mimetypes
import os
import sys
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
DUMP = Path(r"C:/Users/david/Desktop/coding/jak-project/decompiler_out")
OUT = ROOT / "local" / "minimap_icons"
ATLAS = {
    "jak3": DUMP / "jak3_pal/textures/level-default-minimap/mini-map-icons.png",
    "jak2": DUMP / "jak2_pal/textures/ctywide-minimap/mini-map-icons.png",
}
CELL, UP = 20, 8          # the game's cell, and how much bigger the new atlas is
BG = (200, 200, 200, 255)  # the sheets' background
API = "https://openapi.tripo3d.ai/v3"

STYLE = (
    "Redraw these 12 map icons in high definition, same 4x3 grid, order, positions, sizes, silhouettes "
    "and main colors. Style: polished hand-painted HUD icons of Naughty Dog's Jak 3 (PS2): chunky "
    "stylized shapes, soft painted shading lit from the top left, rim highlights, fine material detail "
    "(worn metal, stitched leather, stone grain), rich colors, thick clean dark charcoal outline, smooth "
    "curves, NOT pixel art. Plain "
    "flat light gray background, no text, numbers, frames or shadows. Icons: "
)
SHEETS = [
    [
        "1 olive canvas Wastelander tent, peaked roof, ropes, dark doorway",
        "2 blue steel Precursor tower, crystal spikes at its base, dark door",
        "3 tree with a broad rust red canopy, gnarled brown trunk",
        "4 fist in a brown stitched leather fighting glove",
        "5 red round Metal Head mask, angry glowing yellow eyes",
        "6 gray stone Precursor coin, carved cross and rings, chipped",
        "7 old sage's head: white beard, round goggles, brown hat brim, yellow green collar",
        "8 teal martini glass, thin stem",
        "9 gray stone fortress wall with battlements",
        "10 green round gun sight: ring, cross, tick marks",
        "11 brown wooden palisade with blue steel spikes",
        "12 palm tree, curved ringed trunk, bright green fronds",
    ],
    [
        "1 blue armored chestplate, gold belt, dark straps",
        "2 gray steel gun turret on a square post, barrel right",
        "3 warrior head in profile facing left: long elf ear, dark helmet, pale face",
        "4 purple glowing Dark Eco four-pointed star with a curling tail",
        "5 golden crown, tall points, dark jewels",
        "6 curved scimitar, pale blue blade, dark wrapped hilt, diagonal",
        "7 big gray tire seen from the side, brown metal hub",
        "8 small desert town of sandstone houses, round tower",
        "9 brown rock mound with a dark cave mouth",
        "10 orange red carved metal winged emblem, spread wings, long tail",
        "11 heraldic shield, blue with a diagonal gold and white stripe",
        "12 thick angular yellow lightning bolt",
    ],
    [
        "1 gray city towers and ruins on a sandy hill",
        "2 olive canvas Wastelander tent, peaked roof, dark doorway",
        "3 dark gray metal factory, stepped silhouette, green windows",
        "4 slim ochre lightning bolt",
        "5 gray rocky mountain, two peaks",
        "6 steel broadsword pointing down right, bronze guard",
        "7 dark red round Metal Head mask, angry yellow eyes",
        "8 green ring with a round green gem inside",
        "9 two crossed dark teal blades, an X",
        "10 teal martini glass, thin stem",
        "11 olive goblin face between a red crenellated helmet and a red collar",
        "12 green spoked wheel emblem with a cross sight",
    ],
    [
        "1 gray steel wrench, diagonal",
        "2 dark teal blaster pistol pointing right",
        "3 red armored chestplate, gold belt, dark straps",
        "4 orange fish, big gray eye, facing left",
        "5 gray bone skull, dark eye sockets, teeth",
        "6 teal fishing pole on a stand, red bobber hanging",
        "7 wooden dock frame over blue water",
        "8 bald old engineer, goggles on forehead, brown coat, head and shoulders",
        "9 round brown leather pouch with stitched seams",
        "10 gray metal crate with a pointed antenna on top",
        "11 blue and silver armor plates bound together",
        "12 empty cell",
    ],
]


def cells():
    """The atlas cells with a picture icon: (game, column, row), dots and duplicates left out."""
    dots = {"jak3": [(1, 4), (2, 4), (3, 4), (4, 4), (5, 4)],
            "jak2": [(2, 4), (3, 4), (4, 4), (5, 4), (0, 5), (1, 5)]}
    out = []
    for game, path in ATLAS.items():
        im = Image.open(path).convert("RGBA")
        for j in range(6):
            for i in range(6):
                c = im.crop((i * CELL, j * CELL, (i + 1) * CELL, (j + 1) * CELL))
                if sum(1 for p in c.getdata() if p[3] > 8) < 60 or (i, j) in dots[game]:
                    continue
                out.append((game, i, j))
    return out


def opaque(im):
    """The game's alpha is the PS2's (128 = opaque): to 0..255."""
    r, g, b, a = im.split()
    return Image.merge("RGBA", (r, g, b, a.point(lambda v: min(255, v * 2))))


def ref():
    OUT.mkdir(parents=True, exist_ok=True)
    ims = {g: Image.open(p).convert("RGBA") for g, p in ATLAS.items()}
    all_cells = cells()
    assert len(all_cells) == 47, len(all_cells)
    for s in range(4):
        sheet = Image.new("RGBA", (4 * 256, 3 * 256), BG)
        for k, (g, i, j) in enumerate(all_cells[s * 12:s * 12 + 12]):
            icon = opaque(ims[g].crop((i * CELL, j * CELL, (i + 1) * CELL, (j + 1) * CELL)))
            # smoothed, not big pixels: Tripo copies blocky references as pixel art
            icon = icon.resize((CELL * UP, CELL * UP), Image.BICUBIC).filter(ImageFilter.GaussianBlur(2))
            sheet.alpha_composite(icon, ((k % 4) * 256 + 48, (k // 4) * 256 + 48))
        sheet.convert("RGB").save(OUT / f"ref{s}.png")
    (OUT / "cells.json").write_text(json.dumps(all_cells))
    print(f"reference sheets in {OUT}")


def key():
    k = os.environ.get("TRIPO_API_KEY")
    if not k:
        sys.exit("TRIPO_API_KEY is not set")
    return k


def request(req):
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            out = json.loads(r.read())
    except urllib.error.HTTPError as e:
        sys.exit(f"Tripo error HTTP {e.code}: {e.read().decode()[:400]}")
    if out.get("code") != 0:
        sys.exit(f"Tripo error: {out}")
    return out["data"]


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    return request(urllib.request.Request(API + path, data=data, method=method, headers={
        "Authorization": f"Bearer {key()}", "Content-Type": "application/json"}))


def upload(path):
    boundary = uuid.uuid4().hex
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{path.name}\"\r\n"
            f"Content-Type: {mimetypes.guess_type(path.name)[0]}\r\n\r\n").encode() + path.read_bytes() + \
        f"\r\n--{boundary}--\r\n".encode()
    return request(urllib.request.Request(API + "/files", data=body, method="POST", headers={
        "Authorization": f"Bearer {key()}", "Content-Type": f"multipart/form-data; boundary={boundary}"}))["file_token"]


def image(s):
    token = upload(OUT / f"ref{s}.png")
    prompt = STYLE + "; ".join(SHEETS[s]) + "."
    assert len(prompt) <= 1800, len(prompt)  # Tripo's limit
    bal = float(call("GET", "/account/balance")["balance"])
    print(f"sheet {s}: asking Tripo (balance {bal})")
    tid = call("POST", "/generation/image-to-image",
               {"prompt": prompt, "file": {"type": "png", "file_token": token}})["task_id"]
    while True:
        d = call("GET", f"/tasks/{tid}")
        if d.get("status") == "success":
            break
        if d.get("status") not in ("queued", "running"):
            sys.exit(f"task ended {d.get('status')}: {d.get('error_message')}")
        time.sleep(4)
    url = d["output"].get("generated_image_url") or d["output"].get("image_url")
    with urllib.request.urlopen(url, timeout=300) as r:  # a signed CDN URL, no key sent
        (OUT / f"hd{s}.png").write_bytes(r.read())
    print(f"sheet {s}: {OUT / f'hd{s}.png'}, {d.get('credits_consumed')} credits")


def cut(cell):
    """One redrawn icon: the background flooded away from the cell's border, cropped and centered."""
    im = cell.convert("RGBA")
    w, h = im.size
    bg = im.getpixel((2, 2))
    mask = Image.new("L", (w, h), 0)
    px, mk = im.load(), mask.load()
    near = lambda p: sum(abs(p[c] - bg[c]) for c in range(3)) < 60
    stack = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while stack:
        x, y = stack.pop()
        if 0 <= x < w and 0 <= y < h and not mk[x, y] and near(px[x, y]):
            mk[x, y] = 255
            stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    alpha = Image.eval(mask, lambda v: 255 - v).filter(ImageFilter.GaussianBlur(0.8))
    im.putalpha(alpha)
    box = alpha.point(lambda v: 255 if v > 40 else 0).getbbox()
    return im.crop(box) if box else None


def atlas():
    all_cells = [tuple(c) for c in json.loads((OUT / "cells.json").read_text())]
    size = 128 * UP
    for game, path in ATLAS.items():
        old = Image.open(path).convert("RGBA")
        # only the new drawings go in the repo: the cells not redrawn (the dots, the markers) are left
        # "see-through magenta", which the game fills from its own atlas enlarged
        new = Image.new("RGBA", (size, size), (255, 0, 255, 0))
        for s in range(4):
            sheet_path = OUT / f"hd{s}.png"
            if not sheet_path.exists():
                continue
            # (Tripo answers in its own size, often square: the 4 x 3 cells are cut from that)
            sheet = Image.open(sheet_path).convert("RGBA")
            cw, ch = sheet.width / 4, sheet.height / 3
            for k, (g, i, j) in enumerate(all_cells[s * 12:s * 12 + 12]):
                if g != game:
                    continue
                x, y = (k % 4) * cw, (k // 4) * ch
                icon = cut(sheet.crop((int(x + 6), int(y + 6), int(x + cw - 6), int(y + ch - 6))))
                if icon is None:
                    continue
                # the same size as the game's icon in its cell
                ob = old.crop((i * CELL, j * CELL, (i + 1) * CELL, (j + 1) * CELL)).getbbox()
                bw, bh = (ob[2] - ob[0]) * UP, (ob[3] - ob[1]) * UP
                f = min(bw / icon.width, bh / icon.height)
                icon = icon.resize((max(1, int(icon.width * f)), max(1, int(icon.height * f))), Image.LANCZOS)
                x0, y0 = i * CELL * UP, j * CELL * UP
                new.paste((0, 0, 0, 0), (x0, y0, x0 + CELL * UP, y0 + CELL * UP))  # redrawn: not filled
                cx = x0 + (ob[0] + ob[2]) * UP // 2 - icon.width // 2
                cy = y0 + (ob[1] + ob[3]) * UP // 2 - icon.height // 2
                new.alpha_composite(icon, (cx, cy))
        # back to the PS2's alpha range, as the game's textures have it
        r, g, b, a = new.split()
        done = Image.merge("RGBA", (r, g, b, a.point(lambda v: (v + 1) // 2)))
        done.save(OUT / f"{game}-mini-map-icons.png")
        game_copy = ROOT / "game" / "assets" / "enhanced" / "hud" / game / path.parent.name / "mini-map-icons.png"
        game_copy.parent.mkdir(parents=True, exist_ok=True)
        done.save(game_copy)
        print(f"{game}: {OUT / f'{game}-mini-map-icons.png'} (the game's copy: {game_copy})")


if __name__ == "__main__":
    step = sys.argv[1] if len(sys.argv) > 1 else ""
    if step == "ref":
        ref()
    elif step == "image":
        image(int(sys.argv[2]))
    elif step == "atlas":
        atlas()
    else:
        sys.exit(__doc__)
