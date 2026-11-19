"""Turn the oasis grass painted in Blender (local/blender/oasis_grass.blend) into the dense grass's data.

    blender -b local/blender/oasis_grass.blend -P scripts/enhanced/oasis_grass_from_blend.py [-- --name oasis_user]

Outlines drawn over the map with the Annotate tool (D + drag, placement 3D Cursor or Surface, in the
top view; or any Grease Pencil object's strokes) come first: each stroke is closed from its last point
back to its first, everything inside is full grass, and it thins away over --edge-gap meters past them.
Without outlines, the .blend's grid "Erba" (2 m between its vertices, over a top-down picture of the oasis), whose
vertex group "Erba" is painted in Weight Paint: red (1) where the grass is tall and full, the brush's
soft edge (green-yellow) where it thins away, blue (0) none. Blender X = GOAL x, Blender Y = -GOAL z
(meters): the top view is the game seen from above, not mirrored.

Writes game/assets/enhanced/grass_paint/<name>.json (read by EnhancedGrass.cpp's read_paint/bake_lush):
- "lush": [x, z, r] where the weight is >= --full: the region's outline, a point every --step meters,
  and inside it a few circles (r = the way to the outline) that keep it full without thousands of points;
- "edge": [x, z] just past where the painting ends (weight < --none next to painted vertices), every
  --step meters. Between the two the grass thins and shortens, so a wider soft band gives a gentler edge.
Then sets the oasis zone (the first jak3 zone of grass_paint/user_zones.json, the one near x 560, z 2620)
to paint <name>, its middle and radius covering the painting. Its height (y), edge and water are kept.
"""

import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
PAINT_DIR = ROOT / "game" / "assets" / "enhanced" / "grass_paint"


def args():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--name", default="oasis_user")
    p.add_argument("--object", default="Erba")
    p.add_argument("--full", type=float, default=0.75, help="weight from which the grass is full")
    p.add_argument("--none", type=float, default=0.1, help="weight below which there is none")
    p.add_argument("--step", type=float, default=2.5, help="m between outline points")
    p.add_argument("--edge-gap", type=float, default=12.0, help="m past drawn outlines where the grass is gone")
    p.add_argument("--dry", action="store_true", help="print, write nothing")
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return p.parse_args(argv)


def thin(points, step):
    out = []
    for p in points:
        if not out or np.min(np.hypot(*(np.array(out) - p).T)) >= step:
            out.append(p)
    return out


def boundary(mask):
    """Cells of mask with a 4-neighbour outside it (the grid's border counts as outside)."""
    m = np.pad(mask, 1, constant_values=False)
    inner = m[1:-1, 1:-1] & m[:-2, 1:-1] & m[2:, 1:-1] & m[1:-1, :-2] & m[1:-1, 2:]
    return mask & ~inner


def nearest(points, targets, chunk=4096):
    """Distance from each of points (N x 2) to the nearest of targets (M x 2)."""
    out = np.empty(len(points))
    for i in range(0, len(points), chunk):
        d = points[i:i + chunk, None, :] - targets[None, :, :]
        out[i:i + chunk] = np.sqrt(np.min(np.einsum("ijk,ijk->ij", d, d), axis=1))
    return out


def strokes():
    """The outlines drawn over the map: annotation strokes (Annotate tool, D + drag; scene and view layer
    annotations) and any Grease Pencil object's strokes, each as GOAL (x, z) points (Blender X, -Y)."""
    out = []
    datas = set()
    scene = bpy.context.scene
    for owner in (scene, bpy.context.view_layer, *bpy.data.objects):
        d = getattr(owner, "annotation", None) or getattr(owner, "grease_pencil", None)
        if d is not None and isinstance(d, bpy.types.Annotation):
            datas.add(d)
    datas |= set(getattr(bpy.data, "annotations", []))
    for d in datas:
        for layer in d.layers:
            if getattr(layer, "annotation_hide", False) or getattr(layer, "is_ruler", False):
                continue
            for frame in layer.frames:
                for st in frame.strokes:
                    co = [tuple(p.co) for p in st.points]
                    # (placement "View" keeps a stroke on the screen: its points are in screen space, z 0,
                    # nowhere near the oasis' Y of about -2620; it can't be put back on the map)
                    if co and all(c[2] == 0.0 and -500 < c[1] < 2000 and -500 < c[0] < 4000 for c in co):
                        print(f"skipped a stroke of {layer.info!r} drawn with placement View (screen space): "
                              "redraw it with placement 3D Cursor or Surface")
                        continue
                    out.append([(c[0], -c[1]) for c in co])
    for obj in bpy.data.objects:  # (Grease Pencil objects, Blender 4.3+)
        if obj.type != "GREASEPENCIL" or obj.hide_get():
            continue
        mw = obj.matrix_world
        for layer in obj.data.layers:
            if layer.hide:
                continue
            for frame in layer.frames:
                for st in frame.drawing.strokes:
                    out.append([((mw @ p.position).x, -(mw @ p.position).y) for p in st.points])
    return [s for s in out if len(s) >= 3]


def inside_polygons(X, Z, polys):
    """Even-odd: inside an odd number of the outlines (each closed from its last point to its first)."""
    inside = np.zeros(X.shape, dtype=bool)
    for poly in polys:
        P = np.array(poly)
        hit = np.zeros(X.shape, dtype=bool)
        for (x1, z1), (x2, z2) in zip(P, np.roll(P, -1, axis=0)):
            if z1 == z2:
                continue
            cross = (z1 > Z) != (z2 > Z)
            xi = x1 + (Z - z1) * (x2 - x1) / (z2 - z1)
            hit ^= cross & (X < xi)
        inside ^= hit
    return inside


def from_strokes(polys, step, edge_gap):
    allp = np.concatenate([np.array(p) for p in polys])
    m = edge_gap + 6.0
    xs = np.arange(math.floor(allp[:, 0].min() - m), allp[:, 0].max() + m, step)
    zs = np.arange(math.floor(allp[:, 1].min() - m), allp[:, 1].max() + m, step)
    X, Z = np.meshgrid(xs, zs)
    full = inside_polygons(X, Z, polys)
    bl = np.stack([X[boundary(full)], Z[boundary(full)]], axis=1)
    out = ~full
    d = nearest(np.stack([X[out], Z[out]], axis=1), bl) if len(bl) else np.full(out.sum(), 1e9)
    ring = np.zeros(X.shape, dtype=bool)
    ring[out] = np.abs(d - edge_gap) <= step * 0.75
    return X, Z, full, ring


def from_weights(a):
    obj = bpy.data.objects.get(a.object)
    if obj is None or obj.type != "MESH":
        sys.exit(f"no mesh object {a.object!r} in the .blend (and no outlines drawn)")
    group = obj.vertex_groups.get(a.object) or (obj.vertex_groups[0] if obj.vertex_groups else None)
    if group is None:
        sys.exit(f"{a.object} has no vertex group")
    n, step = int(obj["grid_n"]), float(obj["grid_step"])
    x0, z0 = float(obj["goal_x0"]), float(obj["goal_z0"])
    w = np.zeros((n, n))
    gi = group.index
    mw = obj.matrix_world
    for v in obj.data.vertices:
        p = mw @ v.co
        i, k = round((p.x - x0) / step), round((-p.y - z0) / step)  # (GOAL z = -Blender Y)
        if 0 <= i < n and 0 <= k < n:
            w[k, i] = next((g.weight for g in v.groups if g.group == gi), 0.0)
    X, Z = np.meshgrid(x0 + np.arange(n) * step, z0 + np.arange(n) * step)
    full, painted = w >= a.full, w >= a.none
    # (edge: the unpainted vertices just outside the painting)
    pp = np.pad(painted, 1, constant_values=False)
    ring = ~painted & (pp[:-2, 1:-1] | pp[2:, 1:-1] | pp[1:-1, :-2] | pp[1:-1, 2:])
    return X, Z, full, ring


def main():
    a = args()
    polys = strokes()
    if polys:
        X, Z, full, ring = from_strokes(polys, 2.0, a.edge_gap)
        source = f"{len(polys)} outlines drawn"
    else:
        X, Z, full, ring = from_weights(a)
        source = f"{a.object}'s weights"
    if not full.any():
        sys.exit("nothing marked as full grass: no grass to write")

    # lush: the full region's outline, then circles inside it (biggest first, each where none covers yet)
    bl = np.stack([X[boundary(full)], Z[boundary(full)]], axis=1)
    lush = [(x, z, 0.0) for x, z in thin(list(bl), a.step)]
    outline = len(lush)
    inside = full & ~boundary(full)
    cand = np.stack([X[inside], Z[inside]], axis=1)
    if len(cand):
        r = nearest(cand, bl)
        kept = []  # (x, z, r)
        for j in np.argsort(-r):
            if r[j] < 2.0:
                break
            c = cand[j]
            if kept:
                K = np.array(kept)
                if np.any(np.hypot(K[:, 0] - c[0], K[:, 1] - c[1]) < K[:, 2] - 1.0):
                    continue
            kept.append((c[0], c[1], math.floor(r[j] * 2) / 2))
        lush += kept
    edge = thin(list(np.stack([X[ring], Z[ring]], axis=1)), a.step)

    px, pz = X[full | ring], Z[full | ring]
    cx, cz = (px.min() + px.max()) / 2, (pz.min() + pz.max()) / 2
    radius = float(np.max(np.hypot(px - cx, pz - cz))) + 4.0
    print(f"{source}: {int(full.sum())} full cells -> {len(lush)} lush ({outline} outline), {len(edge)} edge points")
    print(f'zone: x {cx:.1f}, z {cz:.1f}, radius {radius:.1f}, paint "{a.name}"')
    if a.dry:
        return

    out = PAINT_DIR / f"{a.name}.json"
    out.write_text(json.dumps({
        "comment": "Painted in Blender (local/blender/oasis_grass.blend, oasis_grass_from_blend.py): lush, the tall "
                   "full grass (x, z, r: within r of the point); edge, where it has thinned away. Points x, z "
                   "(m, the game's world)",
        "lush": [[round(float(x), 1), round(float(z), 1)] + ([round(float(r), 1)] if r > 0 else [])
                 for x, z, r in lush],
        "edge": [[round(float(x), 1), round(float(z), 1)] for x, z in edge],
    }) + "\n")
    print(f"wrote {out}")

    zones_path = PAINT_DIR / "user_zones.json"
    zones = json.loads(zones_path.read_text())
    jak3 = zones["jak3"]
    oasis = jak3[0]
    if math.hypot(oasis["x"] - 560.0, oasis["z"] - 2620.0) > 150.0:
        sys.exit(f"the first jak3 zone of {zones_path} is not the oasis: {oasis}")
    oasis.update({"x": round(float(cx), 1), "z": round(float(cz), 1), "radius": round(radius, 1), "paint": a.name})
    zones_path.write_text(json.dumps(zones, indent=1) + "\n", newline="\r\n")  # (the file is CRLF)
    print(f"updated {zones_path}: {oasis}")


main()
