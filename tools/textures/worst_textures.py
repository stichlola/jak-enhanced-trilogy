#!/usr/bin/env python3
"""Report the textures that look worst: lowest resolution for how much of the world they cover.

Reads the decompiler's textures (<tpage>/<name>.png), the HD replacements if there are any
(same layout) and the levels' background .glb files (world area and uv area per texture, so real
texels per meter). Textures in no background (characters, effects) are ranked by size and how many
tpages share them. Only reads the game's data; the report goes to the path given, never in the repo.

  python worst_textures.py <report.txt> --textures <decompiler_out>/jak3/textures
      --replacements <checkout>/custom_assets/jak3/texture_replacements [--levels <dir>]
      [--level wascitya] [--target 100] [--top 40] [--no-world]
"""
import argparse
import array
import json
import math
import os
import struct
import sys
from collections import defaultdict

from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# seen from far away or never up close: their low resolution is by design, they count for less
FAR_WORDS = ("lowres", "-low", "low-", "lod", "hills", "peaks", "background", "sky", "horizon", "distant")
# not world surfaces: left out of the "no world data" ranking
SKIP_TPAGE_WORDS = ("minimap", "sprite", "warp")


def image_size(path):
    try:
        with Image.open(path) as im:
            return im.size
    except OSError:
        return None


def scan_textures(root, hd_root):
    """{name: {tpage: (w, h, hd_w, hd_h or None)}}"""
    out = defaultdict(dict)
    for tpage in sorted(os.listdir(root)):
        folder = os.path.join(root, tpage)
        if not os.path.isdir(folder):
            continue
        for fn in os.listdir(folder):
            if not fn.lower().endswith(".png"):
                continue
            size = image_size(os.path.join(folder, fn))
            if not size:
                continue
            hd = None
            if hd_root:
                hd_path = os.path.join(hd_root, tpage, fn)
                if os.path.exists(hd_path):
                    hs = image_size(hd_path)
                    # (a replacement of the same size is no sharper)
                    if hs and hs[0] * hs[1] > size[0] * size[1]:
                        hd = hs
            out[fn[:-4]][tpage] = (size[0], size[1], hd)
    return out


class Glb:
    def __init__(self, path):
        with open(path, "rb") as f:
            magic, _ver, _len = struct.unpack("<III", f.read(12))
            if magic != 0x46546C67:
                raise ValueError("not a glb: " + path)
            jlen, _ = struct.unpack("<II", f.read(8))
            self.j = json.loads(f.read(jlen))
            head = f.read(8)  # (an empty level has no binary chunk)
            self.bin = f.read(struct.unpack("<II", head)[0]) if len(head) == 8 else b""

    def accessor(self, idx):
        a = self.j["accessors"][idx]
        v = self.j["bufferViews"][a["bufferView"]]
        code = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[a["componentType"]]
        comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        arr = array.array(code)
        size = arr.itemsize
        start = v.get("byteOffset", 0) + a.get("byteOffset", 0)
        stride = v.get("byteStride", 0)
        if stride and stride != size * comps:
            raise ValueError("interleaved accessor")
        arr.frombytes(self.bin[start : start + a["count"] * comps * size])
        if sys.byteorder == "big":
            arr.byteswap()
        return arr


def wanted_group(name):
    # the level's own drawn surfaces, once: the highest detail of each (not its far versions)
    if "lowres" in name or "-tie-" in name and not name.endswith("-tie-0"):
        return False
    return "-tfrag-" in name or name.endswith("-tie-0") or name.endswith("-shrub-0")


def prim_areas(glb, prim, cache, mat3):
    """World area (m2) and uv area (tiles) of a triangle-list primitive"""
    attrs = prim["attributes"]
    if "TEXCOORD_0" not in attrs or "indices" not in prim or prim.get("mode", 4) != 4:
        return 0.0, 0.0
    key = (attrs["POSITION"], attrs["TEXCOORD_0"])
    if key not in cache:
        cache[key] = (glb.accessor(attrs["POSITION"]), glb.accessor(attrs["TEXCOORD_0"]))
    pos, uv = cache[key]
    idx = glb.accessor(prim["indices"])
    world = uvs = 0.0
    for t in range(0, len(idx) - 2, 3):
        a, b, c = idx[t], idx[t + 1], idx[t + 2]
        a3, b3, c3 = a * 3, b * 3, c * 3
        ax, ay, az = pos[a3], pos[a3 + 1], pos[a3 + 2]
        ex, ey, ez = pos[b3] - ax, pos[b3 + 1] - ay, pos[b3 + 2] - az
        fx, fy, fz = pos[c3] - ax, pos[c3 + 1] - ay, pos[c3 + 2] - az
        cx, cy, cz = ey * fz - ez * fy, ez * fx - ex * fz, ex * fy - ey * fx
        world += math.sqrt(cx * cx + cy * cy + cz * cz)
        a2, b2, c2 = a * 2, b * 2, c * 2
        u0, v0 = uv[a2], uv[a2 + 1]
        uvs += abs((uv[b2] - u0) * (uv[c2 + 1] - v0) - (uv[c2] - u0) * (uv[b2 + 1] - v0))
    return world * 0.5 * mat3, uvs * 0.5


def node_scale(node):
    """How a node's matrix scales areas (the upper 3x3's mean; the wind's instanced plants)"""
    m = node.get("matrix")
    if not m:
        return 1.0
    sx = math.sqrt(m[0] ** 2 + m[1] ** 2 + m[2] ** 2)
    sy = math.sqrt(m[4] ** 2 + m[5] ** 2 + m[6] ** 2)
    sz = math.sqrt(m[8] ** 2 + m[9] ** 2 + m[10] ** 2)
    return (sx * sy + sy * sz + sz * sx) / 3.0


def read_level(path):
    """{texture name: [world m2, uv tiles]} for one background .glb"""
    glb = Glb(path)
    j = glb.j
    out = defaultdict(lambda: [0.0, 0.0])
    cache = {}
    nodes = j["nodes"]
    for top in j["scenes"][0]["nodes"]:
        group = nodes[top]
        if not wanted_group(group.get("name", "")):
            continue
        for child in group.get("children", []):
            node = nodes[child]
            if "mesh" not in node:
                continue
            scale = node_scale(node)
            for prim in j["meshes"][node["mesh"]]["primitives"]:
                mat = j["materials"][prim["material"]] if "material" in prim else {}
                name = mat.get("name")
                if not name:
                    continue
                w, u = prim_areas(glb, prim, cache, scale)
                out[name][0] += w
                out[name][1] += u
    return out


def pick_tpage(tpages, level):
    """The tpage of this level holding the texture, else the first with it"""
    for t in tpages:
        if level and t.startswith(level + "-"):
            return t
    for t in tpages:
        if t.endswith(("-tfrag", "-alpha", "-shrub", "-pris")):
            return t
    return next(iter(tpages))


def best_hd(tpages):
    best = None
    for t in tpages.values():
        if t[2] and (not best or t[2][0] * t[2][1] > best[0] * best[1]):
            best = t[2]
    return best


def row(name, tpages, level_areas, target):
    world = sum(a[0] for a in level_areas.values())
    uv = sum(a[1] for a in level_areas.values())
    biggest = max(level_areas, key=lambda k: level_areas[k][0])
    tpage = pick_tpage(tpages, biggest)
    w, h, _ = tpages[tpage]
    hd = best_hd(tpages)
    density = math.sqrt(w * h * uv / world) if world > 0 and uv > 0 else 0.0
    eff = density * math.sqrt(hd[0] * hd[1] / (w * h)) if hd else density
    far = any(x in name.lower() for x in FAR_WORDS)
    lack = target / eff - 1.0 if eff > 0 else 0.0
    score = world * max(lack, 0.0) * (0.1 if far else 1.0)
    return {
        "name": name,
        "tpage": tpage,
        "w": w,
        "h": h,
        "hd": hd,
        "world": world,
        "repeats": uv,
        "density": density,
        "eff": eff,
        "far": far,
        "score": score,
        "levels": level_areas,
        "biggest": biggest,
    }


def fmt_row(r):
    hd = "%dx%d" % r["hd"] if r["hd"] else "NO HD"
    levels = "%s%s" % (r["biggest"], " +%d" % (len(r["levels"]) - 1) if len(r["levels"]) > 1 else "")
    return "%10.0f  %9.0f m2  %6.1f t/m  %6.1f eff  %4dx%-4d %-10s x%-7.1f %s%s/%s  [%s]" % (
        r["score"], r["world"], r["density"], r["eff"], r["w"], r["h"], hd, r["repeats"],
        "far: " if r["far"] else "", r["tpage"], r["name"], levels)


HEADER = "     score      world area   texels/m   with HD    size      HD         repeats  tpage/name  [level, +others]"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="report file (outside the repo)")
    ap.add_argument("--textures", required=True, help="decompiler_out/jak3/textures")
    ap.add_argument("--replacements", help="custom_assets/jak3/texture_replacements")
    ap.add_argument("--levels", help="decompiler_out/jak3/levels (default: beside the textures)")
    ap.add_argument("--level", help="one level only")
    ap.add_argument("--target", type=float, default=100.0, help="texels per meter counted as enough (100 = 1 cm)")
    ap.add_argument("--top", type=int, default=40, help="lines of summary printed")
    ap.add_argument("--no-world", action="store_true", help="skip the .glb files: only size and frequency")
    args = ap.parse_args()

    out_path = os.path.abspath(args.out)
    if os.path.commonpath([out_path, REPO]) == REPO:
        sys.exit("the report doesn't go in the repo: " + out_path)

    print("scanning textures...", file=sys.stderr)
    textures = scan_textures(args.textures, args.replacements)
    print("%d texture names" % len(textures), file=sys.stderr)

    # per texture: {level: [world, uv]}
    areas = defaultdict(dict)
    missing = set()
    levels_dir = args.levels or os.path.join(os.path.dirname(os.path.abspath(args.textures)), "levels")
    if not args.no_world and os.path.isdir(levels_dir):
        names = sorted(os.listdir(levels_dir))
        for i, level in enumerate(names):
            path = os.path.join(levels_dir, level, level + "-background.glb")
            if (args.level and level != args.level) or not os.path.exists(path):
                continue
            print("[%d/%d] %s" % (i + 1, len(names), level), file=sys.stderr)
            try:
                for name, a in read_level(path).items():
                    areas[name][level] = a
            except (ValueError, KeyError, OSError) as e:
                print("  skipped: %s" % e, file=sys.stderr)

    rows = []
    for name, level_areas in areas.items():
        level_areas = {k: v for k, v in level_areas.items() if v[0] > 0}
        if not level_areas:
            continue
        if name in textures:
            rows.append(row(name, textures[name], level_areas, args.target))
        else:
            missing.add(name)
    rows.sort(key=lambda r: -r["score"])

    # no world data: by size, then how many tpages share them
    rest = []
    for name, tpages in textures.items():
        if name in areas:
            continue
        use = [t for t in tpages if not any(w in t for w in SKIP_TPAGE_WORDS)]
        if not use:
            continue
        w, h, _ = tpages[use[0]]
        rest.append((w * h, -len(use), name, use[0], w, h, best_hd(tpages)))
    rest.sort()

    no_hd = [r for r in rows if not r["hd"]]
    lines = []
    lines.append("worst textures: %d in the world, %d of them without an HD version; %d with no world data; "
                 "target %.0f texels/m" % (len(rows), len(no_hd), len(rest), args.target))
    lines.append("score: world area x how far under the target (1/10 for far-away textures). t/m: texels per meter.")
    if missing:
        lines.append("%d names in the levels have no png in the textures folder (e.g. %s)" % (len(missing), sorted(missing)[0]))
    summary = list(lines)
    summary.append("")
    summary.append(HEADER)
    summary.extend(fmt_row(r) for r in rows[: args.top])

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n\n== all, worst first\n" + HEADER + "\n")
        f.writelines(fmt_row(r) + "\n" for r in rows)
        f.write("\n== worst without an HD version\n" + HEADER + "\n")
        f.writelines(fmt_row(r) + "\n" for r in no_hd)
        # per level: each texture's own numbers there
        by_level = defaultdict(list)
        for r in rows:
            for level, a in r["levels"].items():
                by_level[level].append((a[0], r, a[1]))
        for level in sorted(by_level):
            f.write("\n== level %s\n" % level)
            scored = []
            for world, r, uv in by_level[level]:
                density = math.sqrt(r["w"] * r["h"] * uv / world) if world > 0 and uv > 0 else 0.0
                eff = density * math.sqrt(r["hd"][0] * r["hd"][1] / (r["w"] * r["h"])) if r["hd"] else density
                lack = args.target / eff - 1.0 if eff > 0 else 0.0
                scored.append((world * max(lack, 0.0) * (0.1 if r["far"] else 1.0), world, density, eff, r))
            scored.sort(key=lambda s: -s[0])
            for s, world, density, eff, r in scored[:25]:
                f.write("%10.0f  %9.0f m2  %6.1f t/m  %6.1f eff  %4dx%-4d %-6s %s/%s\n" % (
                    s, world, density, eff, r["w"], r["h"], "HD" if r["hd"] else "NO HD", r["tpage"], r["name"]))
        f.write("\n== no world data (characters, effects, hfrag...): smallest first, then the most shared\n")
        for area, shared, name, tpage, w, h, hd in rest:
            f.write("%4dx%-4d in %3d tpages  %-6s %s/%s\n" % (w, h, -shared, "HD" if hd else "NO HD", tpage, name))
    print("\n".join(summary))
    print("\nfull report: " + out_path)


if __name__ == "__main__":
    main()
