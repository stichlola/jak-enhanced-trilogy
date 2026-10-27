"""Jak 3's pause maps restyled by Tripo as hand-drawn parchment: the same map, only the look changes.

    python scripts/enhanced/bigmap_tripo.py 2 [5 7 ...] [--model banana_pro]   # ~5 credits a map

Reads local/bigmap/mapNN.png (the game's maps, dumped from the game: not in git) and sends each to
Tripo's image-to-image with an editing model (Gemini's "banana" family keeps the picture's layout and
changes its style), then saves local/bigmap/tripo/mapNN.png at the original's size. Asks nothing:
check the balance and the cost first (cover_art.py balance). The key comes from TRIPO_API_KEY.
"""
import argparse
import sys
import time
import urllib.request
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cover_art as tripo  # noqa: E402  (its upload / call helpers)

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "local/bigmap"
DEST = SRC / "tripo"

PROMPT = (
    "Redraw this exact map in a different art style, changing nothing else. Keep every coastline, "
    "island, river, road, wall, building outline, mountain and marker at exactly the same place, size "
    "and shape, and keep the same framing. Keep the colors: the sea and every river and lake stay "
    "teal blue water, the land keeps its sand and rock tones, glowing orange or green spots keep "
    "glowing, and any lettering already on the map stays where it is. Style: a hand-drawn fantasy "
    "adventure map on aged parchment paper, fine sepia ink outlines and hatching, soft watercolor "
    "washes, subtle paper grain. No new objects, no new text or labels, no compass, no border, no "
    "frame."
)


def restyle(n, model):
    src = SRC / f"map{n:02d}.png"
    im = Image.open(src)
    jpg = SRC / f"map{n:02d}_upload.jpg"
    im.convert("RGB").save(jpg, quality=92)
    token = tripo.upload(jpg)
    tid = tripo.call("POST", "/generation/image-to-image",
                     {"prompt": PROMPT, "model": model, "file": {"type": "jpg", "file_token": token}})["task_id"]
    while True:
        d = tripo.call("GET", f"/tasks/{tid}")
        if d.get("status") == "success":
            break
        if d.get("status") not in ("queued", "running"):
            sys.exit(f"map{n:02d}: task ended {d.get('status')}: {d.get('error_message')}")
        time.sleep(4)
    url = d["output"].get("generated_image_url") or d["output"].get("image_url")
    with urllib.request.urlopen(url, timeout=300) as r:  # a signed CDN URL, no key sent
        raw = DEST / f"map{n:02d}_raw.png"
        raw.write_bytes(r.read())
    # back to the original's size (the model may answer at its own)
    Image.open(raw).convert("RGB").resize(im.size, Image.LANCZOS).save(DEST / f"map{n:02d}.png")
    print(f"map{n:02d}: {DEST / f'map{n:02d}.png'}, {d.get('credits_consumed')} credits", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("maps", nargs="+", type=int)
    ap.add_argument("--model", default="banana_pro")
    a = ap.parse_args()
    DEST.mkdir(parents=True, exist_ok=True)
    print(f"balance {tripo.call('GET', '/account/balance')['balance']}", flush=True)
    for n in a.maps:
        restyle(n, a.model)


if __name__ == "__main__":
    main()
