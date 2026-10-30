"""Jak Enhanced: checks a folder of remade textures against the game's originals, before they go in
custom_assets/<game>/texture_remade (docs/ai-textures.md). Only reads; prints a report.

    python tools/ai_textures/check_textures.py <folder> [--game jak3] [--originals <textures dump>]
                                               [--models <folder of .glb>] [--limit 10]

<folder> holds <name>.png, or <tpage>/<name>.png, plus <name>_n.png / <name>_h.png relief maps. The
originals are the decompiler's dump (decompiler_out/<game>*/textures/<tpage>/<name>.png, made with
"save_texture_pngs": true).

For each picture:
  name    a texture of that name exists (in that page, if it's in one; several different textures
          with one name, in a shared folder: a warning)
  shape   same width : height (the loader refuses another shape); a size that isn't a whole multiple
          of the original is only noted
  alpha   brought down to the original's size, it must match: cutouts in the same place, what's
          opaque still opaque. Plain 0..255 alpha where the game's is the PS2's is noted (the loader
          halves it)
  picture how far the colors moved (only a note: being different is the point) and whether it's
          flipped: an error, when the flipped version compares much closer
  tiling  a texture that repeats (its opposite edges match in the original) must still repeat
  seams   with --models: where the models' UV islands meet, the two sides of each cut must differ no
          more than in the original. The models are the decompiler's .glb (levels/*/*.glb, "rip_levels":
          true). It measures at the vertices of the cuts only, so a seam can still break between them.
          Without --models, seams aren't checked: only alpha and tiling say anything about edges

Exit code 1 if anything is an ERROR. Needs numpy and Pillow.
"""

import argparse
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]

# (the PS2's alpha: 128 is opaque. Merc discards under this much, merc2.frag's 0.128 / 2 of 255)
CUTOUT = 16
ALPHA_TEXELS_BAD = 0.02  # share of texels on the other side of a cutout
ALPHA_MEAN_BAD = 12.0  # mean difference, in 0..128
COLOR_NOTE = 45.0  # mean difference in 0..255 worth a note
TILE_ORIG_MAX = 10.0  # opposite edges this close in the original: it tiles
SEAM_MARGIN = 14.0  # how much worse a cut may get, in 0..255, at its worst tenth
SHAPE_TOLERANCE = 1e-6


def find_originals(game):
    dumps = [folder / "textures" for folder in (ROOT / "decompiler_out").glob(f"{game}*")
             if re.fullmatch(rf"{game}(_[a-z0-9]+)?", folder.name) and (folder / "textures").is_dir()]
    if dumps:
        return max(dumps, key=lambda d: sum(1 for _ in d.glob("*/*.png")))
    raise SystemExit(f"no texture dump of {game} in decompiler_out: pass --originals")


def load_rgba(path):
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32)


def shrink(rgba, w, h):
    """Box-filtered down to w x h (color weighted by nothing: the game's mips aren't either)"""
    image = Image.fromarray(np.clip(rgba + 0.5, 0, 255).astype(np.uint8), "RGBA")
    return np.asarray(image.resize((w, h), Image.BOX), dtype=np.float32)


def edge_mismatch(rgb):
    """Mean difference of opposite edges: left/right, top/bottom"""
    return (float(np.abs(rgb[:, 0] - rgb[:, -1]).mean()), float(np.abs(rgb[0] - rgb[-1]).mean()))


def check_alpha(orig, small, new_max):
    """What the remade alpha does against the original's, at the original's size"""
    notes, errors = [], []
    a0 = orig[..., 3]
    a1 = small[..., 3]
    # (a plain 0..255 picture for a texture in the PS2's: what the loader will make of it)
    if a0.max() <= 128.5 and new_max > 128.5:
        a1 = np.floor((a1 + 1) / 2)
        notes.append("alpha is 0..255 where the game's is 0..128: the loader halves it")
    crossed = float(((a0 < CUTOUT) != (a1 < CUTOUT)).mean())
    mean = float(np.abs(a0 - a1).mean())
    if crossed > ALPHA_TEXELS_BAD:
        errors.append(f"alpha: {crossed * 100:.1f}% of texels moved across the cutout's edge")
    if mean > ALPHA_MEAN_BAD:
        errors.append(f"alpha: differs by {mean:.1f} on average (0..128)")
    elif mean > 2:
        notes.append(f"alpha differs by {mean:.1f} on average (0..128)")
    return notes, errors


def check_picture(orig, small):
    """Weighs where the original is visible only. A flip is the usual way to go wrong: the UVs would
    land on the wrong parts. Notes, errors"""
    seen = orig[..., 3] >= CUTOUT
    if not seen.any():
        return [], []
    diff = float(np.abs(orig[..., :3] - small[..., :3])[seen].mean())
    notes = [f"colors moved by {diff:.0f} of 255"] if diff > COLOR_NOTE else []
    errors = []
    for label, flipped in (("left-right", small[:, ::-1]), ("top-bottom", small[::-1])):
        other = float(np.abs(orig[..., :3] - flipped[..., :3])[seen].mean())
        if other < diff * 0.6:
            errors.append(f"looks flipped {label}: {other:.0f} that way, {diff:.0f} as it is")
    return notes, errors


def check_tiling(orig, small):
    errors = []
    for axis, before, after in zip(("left/right", "top/bottom"), edge_mismatch(orig[..., :3]),
                                   edge_mismatch(small[..., :3])):
        if before < TILE_ORIG_MAX and after > before + 12.0:
            errors.append(f"tiling: {axis} edges matched ({before:.0f}), now {after:.0f} apart")
    return errors


# ---- models: where the UV islands' cuts are

def read_glb(path):
    data = path.read_bytes()
    if data[:4] != b"glTF":
        return None, None
    json_len = struct.unpack("<I", data[12:16])[0]
    doc = json.loads(data[20:20 + json_len])
    return doc, data[20 + json_len + 8:]


def accessor(doc, blob, index):
    acc = doc["accessors"][index]
    view = doc["bufferViews"][acc["bufferView"]]
    count = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    kind = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[acc["componentType"]]
    offset = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride", 0) or count * np.dtype(kind).itemsize
    size = count * np.dtype(kind).itemsize
    if stride == size:
        return np.frombuffer(blob, dtype=kind, count=acc["count"] * count, offset=offset).reshape(-1, count)
    return np.stack([np.frombuffer(blob, dtype=kind, count=count, offset=offset + i * stride)
                     for i in range(acc["count"])])


def material_texture(name):
    return re.sub(r"-envmap$", "", name or "")


def model_cuts(glb):
    """The model's cuts: pairs of (texture, uv) that are one point of the surface"""
    doc, blob = read_glb(glb)
    if not doc or "meshes" not in doc:
        return []
    materials = doc.get("materials", [])
    points = defaultdict(set)
    for mesh in doc["meshes"]:
        for prim in mesh["primitives"]:
            if "indices" not in prim or "material" not in prim or "TEXCOORD_0" not in prim["attributes"]:
                continue
            texture = material_texture(materials[prim["material"]].get("name"))
            used = accessor(doc, blob, prim["indices"]).ravel()
            pos = accessor(doc, blob, prim["attributes"]["POSITION"])
            uv = accessor(doc, blob, prim["attributes"]["TEXCOORD_0"])
            for i in np.unique(used):
                key = tuple(np.round(pos[i], 3).tolist())
                points[key].add((texture, round(float(uv[i][0]), 4), round(float(uv[i][1]), 4)))
    cuts = []
    for sides in points.values():
        sides = sorted(sides)
        for a in range(len(sides)):
            for b in range(a + 1, len(sides)):
                cuts.append((sides[a], sides[b]))
    return cuts


def sample(rgba, u, v):
    """A few texels' average around a UV: a cut's color, not one texel's noise. Row 0 is v = 0"""
    h, w = rgba.shape[:2]
    x = int((u % 1.0) * w) % w
    y = int((v % 1.0) * h) % h
    r = max(1, w // 128)
    ys = np.arange(y - r, y + r + 1) % h
    xs = np.arange(x - r, x + r + 1) % w
    return rgba[np.ix_(ys, xs)][..., :3].mean(axis=(0, 1))


def check_seams(cuts, remade, originals):
    """remade and originals: {texture name: rgba}. {name: (worse, was, is)} over the textures remade"""
    deltas = defaultdict(list)
    for (ta, ua, va), (tb, ub, vb) in cuts:
        if (ta not in remade and tb not in remade) or ta not in originals or tb not in originals:
            continue
        before = np.abs(sample(originals[ta], ua, va) - sample(originals[tb], ub, vb)).mean()
        after = np.abs(sample(remade.get(ta, originals[ta]), ua, va) -
                       sample(remade.get(tb, originals[tb]), ub, vb)).mean()
        for texture in (ta, tb):
            if texture in remade:
                deltas[texture].append((before, after))
    report = {}
    for texture, pairs in deltas.items():
        pairs = np.array(pairs)
        worse = pairs[:, 1] - pairs[:, 0]
        report[texture] = (float(np.percentile(worse, 90)), float(pairs[:, 0].mean()), float(pairs[:, 1].mean()),
                           len(pairs))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--game", default="jak3", choices=("jak1", "jak2", "jak3"))
    parser.add_argument("--originals", type=Path, help="the texture dump (default: decompiler_out/<game>*/textures)")
    parser.add_argument("--models", type=Path, help="a folder of the decompiler's .glb, to check the UV seams")
    parser.add_argument("--limit", type=int, default=0, help="only so many models from --models (0: all)")
    args = parser.parse_args()

    dump = args.originals or find_originals(args.game)
    by_name = defaultdict(list)
    for png in dump.glob("*/*.png"):
        by_name[png.stem].append(png)

    files = sorted(args.folder.rglob("*.png"))
    if not files:
        raise SystemExit(f"no .png in {args.folder}")

    errors_total = warnings_total = 0
    remade_seam, originals_seam = {}, {}
    print(f"{len(files)} pictures in {args.folder}, against {dump}\n")
    for path in files:
        errors, notes = [], []
        stem = path.stem
        relief = stem.endswith(("_n", "_h")) and stem[:-2] in by_name and stem not in by_name
        base = stem[:-2] if relief else stem
        page = path.parent.name if path.parent != args.folder and path.parent.name != "_all" else None
        candidates = by_name.get(base, [])
        if page:
            candidates = [c for c in candidates if c.parent.name == page]

        if not candidates:
            elsewhere = sorted({c.parent.name for c in by_name.get(base, [])})[:4]
            errors.append(f"no texture called {base}" + (f" in page {page}; it's in {', '.join(elsewhere)}"
                                                         if elsewhere else ""))
            print_result(path, args.folder, errors, notes)
            errors_total += 1
            continue

        new = load_rgba(path)
        new_h, new_w = new.shape[:2]
        # (one name can be several textures, of several shapes: the one this picture can be)
        def shape_of(png):
            with Image.open(png) as image:
                return image.size
        fitting = [c for c in candidates if new_w * shape_of(c)[1] == new_h * shape_of(c)[0]]
        orig = load_rgba((fitting or candidates)[0])
        candidates = fitting or candidates
        orig_h, orig_w = orig.shape[:2]
        if abs(new_w * orig_h - new_h * orig_w) > SHAPE_TOLERANCE:
            errors.append(f"shape: {new_w}x{new_h} isn't the shape of the game's {orig_w}x{orig_h}")
        else:
            if new_w % orig_w or new_h % orig_h:
                notes.append(f"{new_w}x{new_h}: not a whole multiple of {orig_w}x{orig_h} (allowed)")
            if new_w < orig_w:
                notes.append(f"{new_w}x{new_h} is smaller than the game's {orig_w}x{orig_h}")

        if relief:
            notes.extend(check_relief(stem, new))
        elif not errors:
            # (one name, several different pictures: a shared folder would dress them all)
            if not page and len(candidates) > 1:
                sizes = {load_rgba(c).tobytes() for c in candidates[:8]}
                if len(sizes) > 1:
                    notes.append(f"WARNING {len(sizes)} different textures have this name: "
                                 f"use {candidates[0].parent.name}/ rather than a shared folder")
            small = shrink(new, orig_w, orig_h)
            alpha_notes, alpha_errors = check_alpha(orig, small, float(new[..., 3].max()))
            notes += alpha_notes
            errors += alpha_errors
            picture_notes, picture_errors = check_picture(orig, small)
            notes += picture_notes
            errors += picture_errors
            errors += check_tiling(orig, small)
            remade_seam[base] = new
            originals_seam[base] = orig

        print_result(path, args.folder, errors, notes)
        errors_total += len(errors)
        warnings_total += sum(1 for n in notes if n.startswith("WARNING"))

    if args.models:
        errors_total += report_seams(args, by_name, remade_seam, originals_seam)
    else:
        print("\nseams: not checked (no --models)")
    print(f"\n{errors_total} errors, {warnings_total} warnings")
    return 1 if errors_total else 0


def check_relief(stem, new):
    notes = []
    if stem.endswith("_n"):
        rgb = new[..., :3] / 127.5 - 1.0
        if rgb[..., 2].mean() < 0.5:
            notes.append("WARNING a normal map should be mostly blue: is it one?")
        if abs(rgb[..., 0].mean()) > 0.2 or abs(rgb[..., 1].mean()) > 0.2:
            notes.append("WARNING this normal map leans to one side on average")
    elif np.abs(new[..., 0] - new[..., 1]).mean() > 8:
        notes.append("note: a height map is read from its red channel")
    return notes


def report_seams(args, by_name, remade, originals):
    """Errors found. Textures the cuts need but that weren't remade come from the dump"""
    models = sorted(args.models.rglob("*.glb"))
    if args.limit:
        models = models[:args.limit]
    wanted = set(remade)
    errors = 0
    results = {}
    print(f"\nseams, from {len(models)} models:")
    for glb in models:
        cuts = [c for c in model_cuts(glb) if c[0][0] in wanted or c[1][0] in wanted]
        if not cuts:
            continue
        for side in {c[0][0] for c in cuts} | {c[1][0] for c in cuts}:
            if side not in originals and side in by_name:
                originals[side] = load_rgba(by_name[side][0])
        for texture, result in check_seams(cuts, remade, originals).items():
            if texture not in results or result[0] > results[texture][0]:
                results[texture] = result + (glb.stem,)
    for texture in sorted(wanted):
        if texture not in results:
            print(f"  {texture}: no cut found on these models")
            continue
        worse, was, now, count, model = results[texture]
        bad = worse > SEAM_MARGIN
        errors += bad
        print(f"  {'ERROR ' if bad else ''}{texture}: {count} points along cuts on {model}: "
              f"{was:.0f} apart in the game, {now:.0f} now; the worst tenth {worse:+.0f}")
    return errors


def print_result(path, folder, errors, notes):
    status = "ERROR" if errors else "ok"
    print(f"{status:5} {path.relative_to(folder)}")
    for line in errors:
        print(f"        {line}")
    for line in notes:
        print(f"        {line}")


if __name__ == "__main__":
    sys.exit(main())
