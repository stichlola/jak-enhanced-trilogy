"""Turn notes painted in photo mode (local/notes/notes.json, EnhancedNotes) into a dense grass
zone's painting (game/assets/enhanced/grass_paint/<name>.json, read by EnhancedGrass).

    python scripts/enhanced/notes_to_grass.py oasis --lush red --edge green

lush: where the grass is tall and full; edge: where it has thinned and shortened away to nothing.
The points are thinned to one every --step meters. Prints the zone's middle and radius for
grass.json.
"""

import argparse
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def thin(points, step):
    out = []
    for p in points:
        if all(math.hypot(p[0] - q[0], p[1] - q[1]) >= step for q in out[-40:]):
            out.append(p)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("--lush", default="red")
    parser.add_argument("--edge", default="green")
    parser.add_argument("--step", type=float, default=2.5)
    parser.add_argument("--notes", default=str(ROOT / "local" / "notes" / "notes.json"))
    args = parser.parse_args()

    notes = json.loads(Path(args.notes).read_text())
    lush, edge = [], []
    for s in notes["strokes"]:
        pts = [(p[0], p[2]) for p in s["points"]]
        if s["color"] == args.lush:
            lush += pts
        elif s["color"] == args.edge:
            edge += pts
    lush, edge = thin(lush, args.step), thin(edge, args.step)
    allp = lush + edge
    cx = (min(p[0] for p in allp) + max(p[0] for p in allp)) / 2
    cz = (min(p[1] for p in allp) + max(p[1] for p in allp)) / 2
    radius = max(math.hypot(p[0] - cx, p[1] - cz) for p in allp) + 6.0
    out = ROOT / "game" / "assets" / "enhanced" / "grass_paint" / f"{args.name}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({
        "comment": "Painted in photo mode (notes_to_grass.py): lush, the tall full grass; edge, where it "
                   "has thinned away. Points x, z (m, the game's world)",
        "lush": [[round(x, 1), round(z, 1)] for x, z in lush],
        "edge": [[round(x, 1), round(z, 1)] for x, z in edge],
    }, indent=None) + "\n")
    print(f"{out}: {len(lush)} lush, {len(edge)} edge points")
    print(f'zone: "x": {cx:.1f}, "z": {cz:.1f}, "radius": {radius:.1f}, "paint": "{args.name}"')


if __name__ == "__main__":
    main()
