"""Scatter the desert props (desert_props.py) around the desert start into placed.json.

    python scripts/enhanced/desert_props_place.py DESERT.fr3 OUT_placed.json BACKGROUND.glb [...]

DESERT.fr3: the extracted desert level, whose hfrag is the dunes' height field (512 x 512, 8 m apart
from the origin: read as jak3-ue5's tools/glb/hfrag_to_glb.py does). BACKGROUND.glb: the ripped
levels around (desert, deserta, desertb, wasdoors; GOAL world space, meters): where any of their
triangles lies over the dunes (rocks, walls, ramps, the city's gate) nothing is put, with a margin.
Each prop stands on the dunes' triangles as the game draws them (a, c, d / a, d, b), tilted a little
toward the slope, turned at random, scaled +-15%; none on steep ground, near the start (the parked
vehicle) or too near another. Seeded: the same files give the same placements. The objects get the
group "desert_props" and replace any of that group already in OUT (the rest kept).
"""

import compression.zstd
import json
import math
import random
import struct
import sys
from pathlib import Path

import numpy as np

EDGE, SPACING, UNITS = 512, 8.0, 4096.0
START = (2266.4, 30.86, 258.1)   # desert-start (level-info.gc), meters
FACING = (0.2, 0.98)             # its camera looks this way (x, z)
CELL = 1.0                       # m: the obstacle grid
REGION = (START[0] - 420, START[0] + 420, START[2] - 420, START[2] + 420)

# model, count, spacing to its own kind (m), clearance from the levels' geometry (m), max slope
# (deg), how much it leans with the slope, sink into the sand (share of its height), collide
KINDS = [
    ("desprop_tree.glb", 8, 14.0, 3.0, 14, 0.15, 0.02, 1),
    ("desprop_cart.glb", 3, 30.0, 3.0, 10, 0.8, 0.0, 1),
    ("desprop_boulder.glb", 13, 7.0, 2.0, 22, 0.5, 0.15, 1),
    ("desprop_shrub.glb", 48, 2.5, 1.0, 22, 0.4, 0.03, 0),
]
HEIGHT = {"desprop_tree.glb": 4.0, "desprop_cart.glb": 1.86, "desprop_boulder.glb": 1.9, "desprop_shrub.glb": 1.0}


def find_vertices(data):
    for offset in range(0, len(data) - 8, 4):
        count = struct.unpack_from("<Q", data, offset)[0]
        if not EDGE <= count <= EDGE * EDGE * 4 or count % 4 or offset + 8 + count * 16 > len(data):
            continue
        ok = True
        for k in [*range(0, count, max(1, count // 64)), count - 1]:
            height, vi, color, u, v, pad = struct.unpack_from("<fIHBBI", data, offset + 8 + k * 16)
            if pad or u > 1 or v > 1 or vi >= EDGE * EDGE or not 0 <= height < 600000 or color >= 4096:
                ok = False
                break
        if ok:
            return offset
    sys.exit("no hfrag vertex array found")


def read_hfrag(path):
    raw = Path(path).read_bytes()
    data = compression.zstd.decompress(raw[8:])
    offset = find_vertices(data)
    count = struct.unpack_from("<Q", data, offset)[0]
    rec = np.frombuffer(data, dtype=np.dtype([("h", "<f4"), ("vi", "<u4"), ("c", "<u2"), ("u", "u1"), ("v", "u1"),
                                              ("pad", "<u4")]), count=count, offset=offset + 8)
    heights = np.full(EDGE * EDGE, np.nan, dtype=np.float64)
    heights[rec["vi"]] = rec["h"] / UNITS
    return heights.reshape(EDGE, EDGE)  # [z][x]


def ground(hf, x, z):
    """Height and normal of the dunes at x, z (meters), on the drawn triangles."""
    gx, gz = x / SPACING, z / SPACING
    ix, iz = int(math.floor(gx)), int(math.floor(gz))
    if not (0 <= ix < EDGE - 1 and 0 <= iz < EDGE - 1):
        return None
    fx, fz = gx - ix, gz - iz
    a, b, c, d = hf[iz, ix], hf[iz, ix + 1], hf[iz + 1, ix], hf[iz + 1, ix + 1]
    if any(math.isnan(v) for v in (a, b, c, d)):
        return None
    # triangles a c d (fz >= fx) and a d b
    if fz >= fx:
        h = a + (d - c) * fx + (c - a) * fz
        dx, dz = (d - c) / SPACING, (c - a) / SPACING
    else:
        h = a + (b - a) * fx + (d - b) * fz
        dx, dz = (b - a) / SPACING, (d - b) / SPACING
    n = np.array([-dx, 1.0, -dz])
    return h, n / np.linalg.norm(n)


def glb_triangles(path):
    d = Path(path).read_bytes()
    jl = struct.unpack_from("<I", d, 12)[0]
    j = json.loads(d[20:20 + jl])
    bin_off = 20 + jl + 8

    def acc(i):
        a = j["accessors"][i]
        bv = j["bufferViews"][a["bufferView"]]
        ct = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[a["componentType"]]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        stride = bv.get("byteStride", 0)
        start = bin_off + bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        if stride and stride != n * np.dtype(ct).itemsize:
            raw = np.frombuffer(d, dtype=np.uint8, count=stride * a["count"], offset=start).reshape(-1, stride)
            return raw[:, :n * np.dtype(ct).itemsize].copy().view(ct).reshape(-1, n)
        return np.frombuffer(d, dtype=ct, count=a["count"] * n, offset=start).reshape(-1, n)

    def node_mats(idx, parent):
        node = j["nodes"][idx]
        m = np.eye(4)
        if "matrix" in node:
            m = np.array(node["matrix"]).reshape(4, 4).T
        else:
            t = node.get("translation", [0, 0, 0])
            s = node.get("scale", [1, 1, 1])
            x, y, z, w = node.get("rotation", [0, 0, 0, 1])
            r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                          [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                          [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
            m[:3, :3] = r * np.array(s)
            m[:3, 3] = t
        m = parent @ m
        yield idx, m
        for c in node.get("children", []):
            yield from node_mats(c, m)

    tris = []
    roots = j["scenes"][j.get("scene", 0)]["nodes"]
    for r in roots:
        for idx, m in node_mats(r, np.eye(4)):
            node = j["nodes"][idx]
            if "mesh" not in node:
                continue
            for p in j["meshes"][node["mesh"]]["primitives"]:
                if p.get("mode", 4) != 4 or "indices" not in p:
                    continue
                pos = acc(p["attributes"]["POSITION"]).astype(np.float64)
                pos = pos @ m[:3, :3].T + m[:3, 3]
                ind = acc(p["indices"]).astype(np.int64).reshape(-1, 3)
                tris.append(pos[ind])
    return np.concatenate(tris) if tris else np.zeros((0, 3, 3))


def obstacle_grid(tris, hf):
    """Cells (1 m) over which some level triangle lies at or above the dunes (less 1 m)."""
    x0, x1, z0, z1 = REGION
    nx, nz = int((x1 - x0) / CELL), int((z1 - z0) / CELL)
    occ = np.zeros((nz, nx), dtype=bool)
    lo, hi = tris.min(1), tris.max(1)
    keep = (hi[:, 0] >= x0) & (lo[:, 0] <= x1) & (hi[:, 2] >= z0) & (lo[:, 2] <= z1)
    tris = tris[keep]
    for t in tris:
        tx0, tx1 = int((t[:, 0].min() - x0) / CELL), int((t[:, 0].max() - x0) / CELL) + 1
        tz0, tz1 = int((t[:, 2].min() - z0) / CELL), int((t[:, 2].max() - z0) / CELL) + 1
        tx0, tz0 = max(tx0, 0), max(tz0, 0)
        tx1, tz1 = min(tx1, nx), min(tz1, nz)
        if tx0 >= tx1 or tz0 >= tz1:
            continue
        xs = x0 + (np.arange(tx0, tx1) + 0.5) * CELL
        zs = z0 + (np.arange(tz0, tz1) + 0.5) * CELL
        X, Z = np.meshgrid(xs, zs)
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = t
        den = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
        if abs(den) < 1e-9:
            # (a wall seen from above: its line, widened to the cell)
            inside = np.ones_like(X, dtype=bool) if (tx1 - tx0) * (tz1 - tz0) <= 4 else np.zeros_like(X, dtype=bool)
            Y = np.full_like(X, t[:, 1].max())
        else:
            w0 = ((bz - cz) * (X - cx) + (cx - bx) * (Z - cz)) / den
            w1 = ((cz - az) * (X - cx) + (ax - cx) * (Z - cz)) / den
            w2 = 1 - w0 - w1
            e = -0.02
            inside = (w0 >= e) & (w1 >= e) & (w2 >= e)
            Y = w0 * ay + w1 * by + w2 * cy
        if not inside.any():
            continue
        # compared with the dunes there (sampled at the triangle's middle: a few m at most)
        g = ground(hf, float(t[:, 0].mean()), float(t[:, 2].mean()))
        gy = g[0] if g else -1e9
        occ[tz0:tz1, tx0:tx1] |= inside & (Y >= gy - 1.0)
    return occ


def near_obstacle(occ, x, z, r):
    x0, _, z0, _ = REGION
    cx, cz = int((x - x0) / CELL), int((z - z0) / CELL)
    k = int(math.ceil(r / CELL))
    a, b = max(cz - k, 0), min(cz + k + 1, occ.shape[0])
    c, d = max(cx - k, 0), min(cx + k + 1, occ.shape[1])
    if a >= b or c >= d:
        return True
    return bool(occ[a:b, c:d].any())


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def rotation(yaw, normal, lean):
    q_yaw = (0.0, math.sin(yaw / 2), 0.0, math.cos(yaw / 2))
    up = np.array([0.0, 1.0, 0.0])
    axis = np.cross(up, normal)
    s = np.linalg.norm(axis)
    if s < 1e-6 or lean <= 0:
        return q_yaw
    ang = math.asin(min(s, 1.0)) * lean
    axis /= s
    q_tilt = (axis[0] * math.sin(ang / 2), axis[1] * math.sin(ang / 2), axis[2] * math.sin(ang / 2), math.cos(ang / 2))
    return quat_mul(q_tilt, q_yaw)


def main():
    fr3, out, glbs = sys.argv[1], Path(sys.argv[2]), sys.argv[3:]
    hf = read_hfrag(fr3)
    tris = np.concatenate([glb_triangles(g) for g in glbs]) if glbs else np.zeros((0, 3, 3))
    occ = obstacle_grid(tris, hf)
    print(f"hfrag ok; {len(tris)} level triangles; {occ.mean() * 100:.1f}% of the region taken")
    rng = random.Random(1234)
    placed, counts = [], {}
    fx, fz = FACING
    for model, count, spacing, clear, max_slope, lean, sink, collide in KINDS:
        n, tries = 0, 0
        while n < count and tries < 40000:
            tries += 1
            # most of them in the view from the start (ahead, 15-160 m), the rest further along
            # the open desert (up to 380 m), shrubs often next to a boulder or a tree
            near_other = [p for p in placed if p["model"] in ("desprop_boulder.glb", "desprop_tree.glb")]
            if model == "desprop_shrub.glb" and near_other and rng.random() < 0.5:
                o = rng.choice(near_other)
                a, r = rng.uniform(0, 2 * math.pi), rng.uniform(2.0, 5.0)
                x, z = o["pos"][0] + math.cos(a) * r, o["pos"][2] + math.sin(a) * r
            else:
                far = rng.random() < 0.3
                r = rng.uniform(160, 380) if far else rng.uniform(15, 160)
                spread = math.pi if far else math.radians(75)
                a = math.atan2(fz, fx) + rng.uniform(-spread, spread)
                x, z = START[0] + math.cos(a) * r, START[2] + math.sin(a) * r
            if math.hypot(x - START[0], z - START[2]) < 15:
                continue
            g = ground(hf, x, z)
            if g is None:
                continue
            h, nrm = g
            if math.degrees(math.acos(min(nrm[1], 1.0))) > max_slope:
                continue
            if near_obstacle(occ, x, z, clear):
                continue
            if any(math.hypot(p["pos"][0] - x, p["pos"][2] - z) < (spacing if p["model"] == model else 2.5)
                   for p in placed):
                continue
            s = rng.uniform(0.85, 1.15)
            q = rotation(rng.uniform(0, 2 * math.pi), nrm, lean)
            y = h - sink * HEIGHT[model] * s
            obj = {"model": model, "pos": [round(x, 2), round(y, 2), round(z, 2)],
                   "rot": [round(v, 4) for v in q], "scale": [round(s, 3)] * 3, "tint": [1.0, 1.0, 1.0],
                   "group": "desert_props"}
            if collide == 0:
                obj["collide"] = 0
            placed.append(obj)
            n += 1
        counts[model] = n
    doc = {}
    if out.exists():
        text = "\n".join(l for l in out.read_text().splitlines() if not l.lstrip().startswith("//"))
        doc = json.loads(text) if text.strip() else {}
    rest = [o for o in doc.get("jak3", []) if o.get("group") != "desert_props"]
    doc["jak3"] = rest + placed
    out.write_text(json.dumps(doc, indent=1) + "\n")
    print("PLACED " + json.dumps(counts))


if __name__ == "__main__":
    main()
