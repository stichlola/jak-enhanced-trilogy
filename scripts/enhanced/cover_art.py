"""Jak Enhanced Trilogy's cover art and app icon, painted by Tripo over the games' own characters.

  python scripts/enhanced/cover_art.py ref               # the reference pictures (free)
  python scripts/enhanced/cover_art.py image cover|icon  # one painted by Tripo (~5 credits each)
  python scripts/enhanced/cover_art.py compose           # title, banner, installer pictures, .ico

`ref` poses Jak 1 (Daxter on his shoulder), Jak II and Jak 3 (each with the Morph Gun) from the games'
models (the decompiler's glbs; Jak 3 from jak3-ue5's staging) and renders them in Blender
(cover_art_blender.py), then lays them on a 2000 x 1500 collage: three panels split by glowing
diagonal seams (a jungle, a neon night city, the desert at sunset), the top kept calm for the title.
Also a 1024 x 1024 icon reference: Jak 3's head with Daxter. These are renders of the game's models:
they stay in local/cover_art and only go to Tripo as references.

`image` sends one of them to Tripo (image to image) to paint it as key art, characters kept as they
are; no text in the picture. `compose` puts our own lettering on the painting (not the games' logo)
and cuts the rest from it: the cover, a 1280 x 640 social banner, the installer's wizard pictures
(.bmp) and the icon (.ico, 16 to 256 px). Without a painting yet it composes the reference, to check
the layout. The Tripo key comes from TRIPO_API_KEY and is never printed.
"""
import json
import mimetypes
import os
import random
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent
OUT = ROOT / "local" / "cover_art"
DUMP = ROOT / "decompiler_out"
UE = ROOT.parent / "jak3-ue5" / "tools" / "staging"
API = "https://openapi.tripo3d.ai/v3"
W, H = 2000, 1500
TITLE = 0.28  # the top of the cover kept calm for the title

# the characters: the parts posed together (see cover_art_blender.py), the camera, the rim light
CHARACTERS = {
    "jak1": {"yaw": 22, "rim": (1.0, 0.85, 0.45), "rim2": (0.45, 0.75, 1.0), "parts": [
        {"glb": DUMP / "jak1/levels/common/eichar-lod0.glb", "action": "eichar-stance-loop"},
        {"glb": DUMP / "jak1/levels/common/sidekick-lod0.glb", "action": "sidekick-stance-loop",
         "ride": "LshoulderPad"}]},
    "jak2": {"yaw": 28, "rim": (0.4, 0.55, 1.0), "rim2": (0.9, 0.35, 1.0), "parts": [
        {"glb": DUMP / "jak2/levels/common/jakb-lod0.glb", "action": "jakb-gun-stance"},
        {"glb": DUMP / "jak2/levels/common/gun-lod0.glb", "action": "gun-idle-red", "attach": "gun"}]},
    "jak3": {"yaw": -22, "rim": (1.0, 0.6, 0.25), "rim2": (1.0, 0.8, 0.5), "parts": [
        {"glb": UE / "jak/jak.glb", "action": "jakb-gun-stance"},
        {"glb": UE / "gun/gun.glb", "action": "gun-idle-red", "attach": "gun"}]},
    "icon": {"yaw": 12, "pitch": 4, "frame_to": "head", "zoom": 0.62, "rim": (1.0, 0.65, 0.3),
             "rim2": (0.5, 0.75, 1.0), "parts": [
        {"glb": UE / "jak/jak.glb", "action": "jakb-stance-loop"},
        {"glb": UE / "daxter/daxter.glb", "action": "daxter-stance-loop", "ride": "LshoulderPad"}]},
}

STYLE = (
    "Repaint this picture as polished painted video game key art, a box cover: rich hand-painted "
    "digital illustration, dramatic cinematic lighting, strong colored rim lights, soft volumetric "
    "glow, crisp detail on faces, cloth, leather and metal. Keep every character EXACTLY as in the "
    "picture: the same faces, hair, ears, goggles, outfits, colors, proportions, poses, guns and "
    "places in the frame; only the rendering becomes painted and more detailed. "
)
PROMPTS = {
    "cover": STYLE + (
        "Three vertical panels split by two glowing diagonal lightning seams (blue on the left, red on "
        "the right). Left: young Jak with spiky blond-green hair, blue tunic, with Daxter, an orange "
        "otter-weasel with goggles, on his shoulder, in a lush tropical jungle with ancient stone ruins, "
        "golden light and crackling blue eco sparks. Middle: Jak II with long green hair, goatee, blue "
        "armor and red scarf, holding the gun, in a dark futuristic city at night, blue and purple neon. "
        "Right: Jak 3 with a red scarf over his face, brown desert armor, holding the gun, in a desert "
        "at sunset, orange sky, sand dunes, rusted wrecks. Keep the top quarter of the picture as calm "
        "sky and background with nothing important in it. Absolutely no text, letters, logo or watermark."),
    "icon": STYLE + (
        "An app icon: Jak's head, long green hair, brown goggles with a red lens, pointed ears, red "
        "scarf, with Daxter (orange otter-weasel with goggles) on his shoulder, bold readable shapes, "
        "thick clean dark outline around the characters, big shapes that read at a small size, warm "
        "sunset orange and blue background glow. No text, letters, logo or watermark."),
}


def blender():
    for p in [os.environ.get("BLENDER", ""),
              r"C:/Program Files/Blender Foundation/Blender 5.1/blender.exe",
              r"C:/Program Files/Blender Foundation/Blender 5.0/blender.exe",
              r"C:/Program Files/Blender Foundation/Blender 4.5/blender.exe"]:
        if p and Path(p).exists():
            return p
    sys.exit("Blender not found (set BLENDER)")


def render(name, size):
    spec = dict(CHARACTERS[name])
    spec["parts"] = [{**p, "glb": str(p["glb"])} for p in spec["parts"]]
    spec["out"] = str(OUT / f"{name}.png")
    spec["size"] = size
    job = OUT / f"{name}.json"
    job.write_text(json.dumps(spec, indent=1))
    print(f"rendering {name}...", flush=True)
    r = subprocess.run([blender(), "-b", "--factory-startup", "-P", str(HERE / "cover_art_blender.py"), "--",
                        str(job)], capture_output=True, text=True)
    if "RENDERED" not in r.stdout:
        print(r.stdout[-3000:], r.stderr[-2000:])
        sys.exit(f"{name}: the render failed")
    return Image.open(spec["out"]).convert("RGBA")


def gradient(size, top, bottom):
    w, h = size
    col = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        col.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    return col.resize((w, h))


def panel_jungle(rng):
    im = gradient((W, H), (70, 110, 90), (150, 130, 50))
    d = ImageDraw.Draw(im)
    d.ellipse((150, 250, 750, 850), fill=(240, 210, 120))  # the sun through the canopy
    for _ in range(14):  # trunks and ruins
        x = rng.randint(-50, 800)
        d.rectangle((x, rng.randint(200, 600), x + rng.randint(30, 90), H), fill=(40, 60, 35))
    for _ in range(40):  # leaves
        x, y = rng.randint(-100, 900), rng.randint(-100, 700)
        r = rng.randint(60, 180)
        d.ellipse((x - r, y - r // 2, x + r, y + r // 2), fill=(30 + rng.randint(0, 40), 90 + rng.randint(0, 60), 40))
    d.rectangle((0, 1250, W, H), fill=(60, 95, 40))
    for _ in range(60):  # blue eco sparks
        x, y = rng.randint(0, 800), rng.randint(500, 1300)
        d.line((x, y, x + rng.randint(-40, 40), y + rng.randint(-40, 40)), fill=(120, 200, 255), width=3)
    return im


def panel_city(rng):
    im = gradient((W, H), (20, 25, 70), (60, 30, 90))
    d = ImageDraw.Draw(im)
    for _ in range(16):  # towers with lit windows
        x = rng.randint(500, 1500)
        top = rng.randint(150, 700)
        w = rng.randint(80, 220)
        d.rectangle((x, top, x + w, H), fill=(25, 28, 50))
        for _ in range(25):
            wx, wy = rng.randint(x + 5, x + w - 15), rng.randint(top + 10, H - 300)
            d.rectangle((wx, wy, wx + 10, wy + 16), fill=rng.choice([(90, 160, 255), (230, 80, 220), (90, 230, 160)]))
    d.rectangle((0, 1250, W, H), fill=(30, 30, 45))
    return im


def panel_desert(rng):
    im = gradient((W, H), (230, 140, 60), (200, 120, 60))
    d = ImageDraw.Draw(im)
    d.ellipse((1550, 300, 1850, 600), fill=(255, 220, 150))
    for i in range(6):  # dunes
        y = 900 + i * 90
        d.ellipse((1100 - i * 120, y, 2300 + i * 80, y + 700), fill=(210 - i * 12, 140 - i * 10, 70 - i * 6))
    d.polygon([(1700, 1150), (1900, 1100), (1980, 1180), (1720, 1220)], fill=(90, 60, 40))  # a wreck
    return im


def seams(mask_x0, mask_x1):
    """The panels' borders: two lines leaning right, as x at the top and at the bottom."""
    return [((mask_x0, 0), (mask_x0 - 180, H)), ((mask_x1, 0), (mask_x1 - 180, H))]


def collage(chars):
    rng = random.Random(7)
    layers = [panel_jungle(rng), panel_city(rng), panel_desert(rng)]
    lines = seams(760, 1440)
    out = layers[2].copy()
    for i in (1, 0):  # the city, then the jungle, each left of its seam
        (xt, _), (xb, _) = lines[i]
        m = Image.new("L", (W, H), 0)
        ImageDraw.Draw(m).polygon([(0, 0), (xt, 0), (xb, H), (0, H)], fill=255)
        out.paste(layers[i], (0, 0), m)
    out = out.filter(ImageFilter.GaussianBlur(6)).convert("RGBA")
    # the characters, feet near the bottom, heads under the title
    centers = [430, 1010, 1600]
    for img, cx in zip(chars, centers):
        box = img.getbbox()
        img = img.crop(box)
        h = int(H * (1 - TITLE) * 0.98)
        img = img.resize((int(img.width * h / img.height), h), Image.LANCZOS)
        out.alpha_composite(img, (cx - img.width // 2, H - h - 10))
    # the glowing seams over everything
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(glow)
    for (a, b), color in zip(lines, [(80, 170, 255), (255, 70, 60)]):
        d.line([a, b], fill=color + (255,), width=26)
    glow = glow.filter(ImageFilter.GaussianBlur(14))
    d = ImageDraw.Draw(glow)
    for a, b in lines:
        d.line([a, b], fill=(255, 255, 255, 255), width=6)
    out.alpha_composite(glow)
    return out


def icon_ref(head):
    bg = Image.new("RGBA", (1024, 1024), (40, 55, 130, 255))
    for r in range(724, 0, -4):  # a sunset glow from the middle
        t = r / 724
        c = (int(255 - 215 * t), int(175 - 120 * t), int(80 + 50 * t), 255)
        ImageDraw.Draw(bg).ellipse((512 - r, 560 - r, 512 + r, 560 + r), fill=c)
    head = head.crop(head.getbbox())
    s = 940 / max(head.size)
    head = head.resize((int(head.width * s), int(head.height * s)), Image.LANCZOS)
    bg.alpha_composite(head, ((1024 - head.width) // 2, 1024 - head.height))
    return bg


def cmd_ref():
    OUT.mkdir(parents=True, exist_ok=True)
    chars = [render(n, (900, 1350)) for n in ("jak1", "jak2", "jak3")]
    collage(chars).convert("RGB").save(OUT / "cover_ref.png")
    icon_ref(render("icon", (1024, 1024))).convert("RGB").save(OUT / "icon_ref.png")
    print(f"{OUT / 'cover_ref.png'}\n{OUT / 'icon_ref.png'}")


# --- Tripo (as minimap_icons.py)
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


def cmd_image(which):
    ref = OUT / f"{which}_ref.png"
    if which not in PROMPTS or not ref.exists():
        sys.exit("usage: image cover|icon (after ref)")
    prompt = PROMPTS[which]
    assert len(prompt) <= 1800, len(prompt)  # Tripo's limit
    # (a JPEG upload: the 2000 x 1500 PNG is large)
    jpg = OUT / f"{which}_ref_upload.jpg"
    Image.open(ref).convert("RGB").save(jpg, quality=92)
    token = upload(jpg)
    bal = float(call("GET", "/account/balance")["balance"])
    print(f"{which}: asking Tripo (balance {bal})", flush=True)
    tid = call("POST", "/generation/image-to-image",
               {"prompt": prompt, "file": {"type": "jpg", "file_token": token}})["task_id"]
    while True:
        d = call("GET", f"/tasks/{tid}")
        if d.get("status") == "success":
            break
        if d.get("status") not in ("queued", "running"):
            sys.exit(f"task ended {d.get('status')}: {d.get('error_message')}")
        time.sleep(4)
    url = d["output"].get("generated_image_url") or d["output"].get("image_url")
    n = 1
    while (OUT / f"tripo_{which}_{n}.png").exists():
        n += 1
    dest = OUT / f"tripo_{which}_{n}.png"
    with urllib.request.urlopen(url, timeout=300) as r:  # a signed CDN URL, no key sent
        dest.write_bytes(r.read())
    print(f"{which}: {dest}, {d.get('credits_consumed')} credits")


# --- compose
FONT_DIR = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"


def font(size, names=("ariblk.ttf", "impact.ttf", "arialbd.ttf")):
    for n in names:
        if (FONT_DIR / n).exists():
            return ImageFont.truetype(str(FONT_DIR / n), size)
    return ImageFont.load_default(size)


def lettering(text, size, top, bottom, outline=(20, 16, 30), spacing=0.0):
    """Text filled with a vertical gradient, a dark outline and a soft drop shadow (RGBA, cropped)."""
    f = font(size)
    stroke = max(4, size // 14)
    box = ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), text, font=f, stroke_width=stroke)
    w, h = box[2] - box[0] + stroke * 4, box[3] - box[1] + stroke * 4
    origin = (stroke * 2 - box[0], stroke * 2 - box[1])
    fill_mask = Image.new("L", (w, h))
    ImageDraw.Draw(fill_mask).text(origin, text, font=f, fill=255)
    edge_mask = Image.new("L", (w, h))
    ImageDraw.Draw(edge_mask).text(origin, text, font=f, fill=255, stroke_width=stroke)
    out = Image.new("RGBA", (w + 24, h + 24))
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    shadow.putalpha(edge_mask.filter(ImageFilter.GaussianBlur(8)).point(lambda v: v * 0.7))
    out.alpha_composite(shadow, (14, 18))
    edge = Image.new("RGBA", (w, h), outline + (255,))
    edge.putalpha(edge_mask)
    out.alpha_composite(edge, (0, 0))
    grad = gradient((w, h), top, bottom).convert("RGBA")
    # a bright band across the upper third: a metal sheen
    sheen = Image.new("L", (w, h))
    ImageDraw.Draw(sheen).rectangle((0, int(h * 0.25), w, int(h * 0.42)), fill=70)
    grad = Image.composite(Image.new("RGBA", (w, h), (255, 255, 255, 255)), grad, sheen.filter(ImageFilter.GaussianBlur(6)))
    grad.putalpha(fill_mask)
    out.alpha_composite(grad, (0, 0))
    return out.crop(out.getbbox())


def title_block(width):
    """JAK big, ENHANCED TRILOGY stacked beside it (our own lettering, not the games' logo)."""
    jak = lettering("JAK", 330, (255, 214, 90), (226, 92, 24))
    enh = lettering("ENHANCED", 150, (255, 236, 170), (196, 140, 40))
    tri = lettering("TRILOGY", 150, (200, 230, 255), (60, 120, 220))
    right_w = max(enh.width, tri.width)
    gap = 40
    block = Image.new("RGBA", (jak.width + gap + right_w, max(jak.height, enh.height + tri.height + 10)))
    block.alpha_composite(jak, (0, (block.height - jak.height) // 2))
    y = (block.height - enh.height - tri.height - 10) // 2
    block.alpha_composite(enh, (jak.width + gap + (right_w - enh.width) // 2, y))
    block.alpha_composite(tri, (jak.width + gap + (right_w - tri.width) // 2, y + enh.height + 10))
    if block.width > width:
        s = width / block.width
        block = block.resize((int(block.width * s), int(block.height * s)), Image.LANCZOS)
    return block


def latest(which):
    painted = sorted(OUT.glob(f"tripo_{which}_*.png"), key=lambda p: p.stat().st_mtime)
    if painted:
        return painted[-1], True
    return OUT / f"{which}_ref.png", False


def cover_crop(img, aspect):
    """The biggest crop of this aspect, centered (the cover is 4:3)."""
    w, h = img.size
    if w / h > aspect:
        nw = int(h * aspect)
        return img.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
    nh = int(w / aspect)
    return img.crop((0, 0, w, nh))


def save_bmp(img, path, size):
    img.convert("RGB").resize(size, Image.LANCZOS).save(path, "BMP")


def cmd_compose():
    dest = OUT / "out"
    dest.mkdir(parents=True, exist_ok=True)
    src, painted = latest("cover")
    if not painted:
        print(f"no painting yet: composing the reference ({src.name}) to check the layout")
    art = cover_crop(Image.open(src).convert("RGBA"), W / H).resize((W, H), Image.LANCZOS)
    cover = art.copy()
    title = title_block(int(W * 0.86))
    cover.alpha_composite(title, ((W - title.width) // 2, int(H * TITLE / 2 - title.height / 2) + 10))
    cover.convert("RGB").save(dest / "cover.png")
    cover.convert("RGB").save(dest / "cover.jpg", quality=92)
    # the social banner (2:1): the title and the faces
    banner = cover.resize((1280, 960), Image.LANCZOS).crop((0, 20, 1280, 660))
    banner.convert("RGB").save(dest / "banner-1280x640.png")
    # the installer's side picture (welcome and last page, 164 x 314 at 100%, 328 x 628 at 200%):
    # the middle panel, no title
    tall = art.crop((W // 2 - 400, int(H * TITLE) - 120, W // 2 + 400, H))
    tall = cover_crop(tall, 164 / 314)
    save_bmp(tall, dest / "wizard-164x314.bmp", (164, 314))
    save_bmp(tall, dest / "wizard-328x628.bmp", (328, 628))
    # the icon: the painted (or reference) head, rounded corners
    src, painted_icon = latest("icon")
    if not painted_icon:
        print(f"no icon painting yet: using {src.name}")
    ic = cover_crop(Image.open(src).convert("RGBA"), 1.0).resize((1024, 1024), Image.LANCZOS)
    mask = Image.new("L", (1024, 1024))
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 1023, 1023), radius=190, fill=255)
    ic.putalpha(ImageChops.multiply(ic.getchannel("A"), mask))
    ic.save(dest / "icon-1024.png")
    ic.save(dest / "jak-enhanced.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    save_bmp(Image.alpha_composite(Image.new("RGBA", ic.size, (255, 255, 255, 255)), ic),
             dest / "wizard-small-55x55.bmp", (55, 55))
    save_bmp(Image.alpha_composite(Image.new("RGBA", ic.size, (255, 255, 255, 255)), ic),
             dest / "wizard-small-110x110.bmp", (110, 110))
    for p in sorted(dest.iterdir()):
        print(p)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("ref", "image", "compose", "balance"):
        sys.exit(__doc__)
    if sys.argv[1] == "ref":
        cmd_ref()
    elif sys.argv[1] == "image":
        cmd_image(sys.argv[2] if len(sys.argv) > 2 else "")
    elif sys.argv[1] == "balance":
        print(call("GET", "/account/balance")["balance"])
    else:
        cmd_compose()


if __name__ == "__main__":
    main()
