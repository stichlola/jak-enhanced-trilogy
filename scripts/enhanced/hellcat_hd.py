"""Jak Enhanced: a higher-detail Hellcat (or another rigid merc model) for merc_replacements.

    blender -b --factory-startup -P scripts/enhanced/hellcat_hd.py -- <ripped.glb> <textures dir> <out.glb>
        [--levels 3] [--split-angle 6] [--max-fold 50] [--bevel 0.012] [--bevel-angle 30] [--ao 0.35]
        [--preview <png> [--cam x,y,z] [--look x,y,z]]   (a Workbench render, the nose toward -y, z up)

<ripped.glb>: the decompiler's rip of the model (rip_levels, e.g. decompiler_out/jak3/levels/ctycarc/
hellcat-lod0.glb). <textures dir>: hellcat_hd_textures.py's output, <name>.png per texture of the rip.
Writes <out.glb> for custom_assets/jak3/merc_replacements, used with {"joints_by_name": true,
"all_lods": true} (docs/vehicle-models.md).

What it does, keeping the shape, the joints and the UVs:
- builds the mesh straight from the glb's buffers, in the glTF frame (no axis conversion, no
  armature import: the skeleton nodes and skin are copied over as they are, so nothing can move);
- welds each rigid part (vertices grouped by their main joint, so the turret never sticks to the
  body), drops degenerate and doubled triangles;
- rounds the curves with an interpolating subdivision (Phong tessellation on the game's own normals):
  an edge is split where the artist's normals at its ends disagree (a curve: the hull, the engines,
  the domes) and only where they are smooth across it, the new point pushed onto the curve those
  normals describe. The game's vertices never move and flat panels and hard edges stay as they were,
  so the silhouette can't shrink or melt as with Catmull-Clark. UVs, colours, normals and weights are
  interpolated inside each game triangle, so the textures sit exactly where they did;
- bevels the hard edges (the game's normals split across them, and they fold more than --bevel-angle)
  a centimetre or so (2 segments, normals interpolated: hardened ones made curved panels next to a
  bevel flat-shaded): the corners catch the light;
- keeps a bulge from passing through a surface just over it (the game layers some panels a few
  millimetres apart: two layers bulging differently would cross in dark patches);
- multiplies the game's baked vertex shading by an ambient occlusion computed on the new mesh
  (ray cast, per vertex), so seams, vents and the cockpit get contact shadows;
- writes one primitive per texture with its own vertices (the decompiler reads every vertex of a
  primitive's accessors: shared ones would be copied into each draw). The textures' alpha is
  hellcat_hd_textures.py's, already in the PC's range.
"""
import json
import math
import random
import struct
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

argv = sys.argv[sys.argv.index("--") + 1:]
SRC, TEX_DIR, OUT = (Path(a).resolve() for a in argv[:3])


def option(name, default):
    if name in argv:
        return type(default)(argv[argv.index(name) + 1])
    return default


LEVELS = option("--levels", 3)
SPLIT_ANGLE = math.radians(option("--split-angle", 6.0))
BEVEL = option("--bevel", 0.012)
BEVEL_ANGLE = math.radians(option("--bevel-angle", 30.0))
AO = option("--ao", 0.35)
PREVIEW = option("--preview", "")
WELD = 0.0005
PHONG = 0.75
GAP = 0.01
MAX_FOLD = math.radians(option("--max-fold", 50.0))

# --- read the rip -------------------------------------------------------------------------------
data = SRC.read_bytes()
json_len = struct.unpack("<I", data[12:16])[0]
gltf = json.loads(data[20:20 + json_len])
BIN = 20 + json_len + 8
TYPES = {5126: np.float32, 5121: np.uint8, 5123: np.uint16, 5125: np.uint32}
SIZES = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def accessor(i):
    a = gltf["accessors"][i]
    view = gltf["bufferViews"][a["bufferView"]]
    n = SIZES[a["type"]]
    start = BIN + view.get("byteOffset", 0) + a.get("byteOffset", 0)
    arr = np.frombuffer(data, TYPES[a["componentType"]], a["count"] * n, start).reshape(a["count"], n)
    if a.get("normalized"):
        arr = arr.astype(np.float32) / np.iinfo(arr.dtype).max
    return arr.astype(np.float64)


skin = gltf["skins"][0]
joint_names = [gltf["nodes"][j]["name"] for j in skin["joints"]]
mesh_node = next(n for n in gltf["nodes"] if "mesh" in n)


def texture_name(mat_idx):
    mat = gltf["materials"][mat_idx]
    tex = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture")
    if tex is None:
        return None
    return gltf["images"][gltf["textures"][tex["index"]]["source"]]["name"]


def unit(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-12)


# triangles with their corners' data; vertices welded per rigid part
n_joints = len(joint_names)
vert_id, vert_pos, vert_w = {}, [], []
tri_v, tri_n, tri_uv, tri_col, tri_mat = [], [], [], [], []
tex_names, tex_blend = [], {}
src_tris = 0
for prim in gltf["meshes"][mesh_node["mesh"]]["primitives"]:
    name = texture_name(prim["material"])
    if name is None:
        continue
    if name not in tex_names:
        tex_names.append(name)
    tex_blend[name] = gltf["materials"][prim["material"]].get("alphaMode") == "BLEND"
    attrs = prim["attributes"]
    pos = accessor(attrs["POSITION"])
    nrm = unit(accessor(attrs["NORMAL"]))
    uv = accessor(attrs["TEXCOORD_0"])
    col = accessor(attrs["COLOR_0"]) if "COLOR_0" in attrs else np.full((len(pos), 4), 0.5)
    jnt = accessor(attrs["JOINTS_0"]).astype(np.int64)
    wgt = accessor(attrs["WEIGHTS_0"])
    idx = accessor(prim["indices"]).astype(np.int64).ravel()
    dense = np.zeros((len(pos), n_joints))
    for k in range(4):
        np.add.at(dense, (np.arange(len(pos)), jnt[:, k]), wgt[:, k])
    part = dense.argmax(1)
    for t in range(0, len(idx) - 2, 3):
        tri = idx[t:t + 3]
        src_tris += 1
        p0, p1, p2 = pos[tri]
        if np.dot(np.cross(p1 - p0, p2 - p0), nrm[tri].sum(0)) < 0:
            # (strips: the winding isn't the facing; the game's normals are)
            tri = tri[[0, 2, 1]]
        ids = []
        for i in tri:
            key = (int(part[i]), *np.round(pos[i] / WELD).astype(np.int64).tolist())
            if key not in vert_id:
                vert_id[key] = len(vert_pos)
                vert_pos.append(pos[i])
                vert_w.append(dense[i])
            ids.append(vert_id[key])
        tri_v.append(ids)
        tri_n.append(nrm[tri])
        tri_uv.append(uv[tri])
        tri_col.append(col[tri])
        tri_mat.append(tex_names.index(name))

# degenerate ones (welded corners) and the same triangle twice (the game draws some both ways)
seen, keep = set(), []
for t, ids in enumerate(tri_v):
    key = (tuple(sorted(ids)), tri_mat[t])
    if len(set(ids)) == 3 and key not in seen:
        seen.add(key)
        keep.append(t)
V = [np.array(p) for p in vert_pos]
W = [np.array(w) for w in vert_w]
TV = [list(tri_v[t]) for t in keep]
TN = [list(tri_n[t]) for t in keep]
TUV = [list(tri_uv[t]) for t in keep]
TC = [list(tri_col[t]) for t in keep]
TM = [tri_mat[t] for t in keep]
game_pos = np.array(vert_pos)
game_tris = [tuple(tri_v[t]) for t in keep]
print(f"[hellcat-hd] rip: {src_tris} triangles, {len(tex_names)} textures; welded {len(V)} vertices, "
      f"{len(TV)} triangles")


def angle(a, b):
    return math.acos(max(-1.0, min(1.0, float(np.dot(a, b)))))


# --- interpolating subdivision ---------------------------------------------------------------------
for level in range(LEVELS):
    edges = {}
    for t, ids in enumerate(TV):
        for k in range(3):
            a, b = ids[k], ids[(k + 1) % 3]
            edges.setdefault((min(a, b), max(a, b)), []).append((t, k))
    # each edge: smooth in the game (its normals don't split across it) and how much it folds
    vp = np.array(V)
    tv = np.array(TV)
    fn = unit(np.cross(vp[tv[:, 1]] - vp[tv[:, 0]], vp[tv[:, 2]] - vp[tv[:, 0]]))
    info = {}
    fold_at = np.zeros(len(V))
    for (a, b), users in edges.items():
        if len(users) != 2:
            continue  # an open border or a fin: stays straight
        (t1, k1), (t2, k2) = users

        def ends(t, k):
            n0, n1 = TN[t][k], TN[t][(k + 1) % 3]
            return (n0, n1) if TV[t][k] == a else (n1, n0)

        na, nb = ends(t1, k1)
        na2, nb2 = ends(t2, k2)
        if angle(na, na2) > math.radians(3) or angle(nb, nb2) > math.radians(3):
            continue  # a hard edge: the game's normals split across it
        fold = angle(fn[t1], fn[t2])
        info[(a, b)] = (na, nb, fold)
        fold_at[a] = max(fold_at[a], fold)
        fold_at[b] = max(fold_at[b], fold)
    # (the mesh as it is, to keep a bulge from going through a surface just over it: the game
    # layers panels a few millimetres apart, and two layers bulging differently cross in patches)
    tree = BVHTree.FromPolygons([Vector(p) for p in V], [tuple(ids) for ids in TV], all_triangles=True)
    mid = {}
    for (a, b), (na, nb, fold) in info.items():
        turn = angle(na, nb)
        # (a curve: its normals turn along the edge, and nothing around folds like a box corner the
        # artist only smoothed, which would balloon)
        if turn < SPLIT_ANGLE or fold > MAX_FOLD or fold_at[a] > MAX_FOLD or fold_at[b] > MAX_FOLD:
            continue
        pa, pb = V[a], V[b]
        m = 0.5 * (pa + pb)
        proj_a = m - np.dot(m - pa, na) * na
        proj_b = m - np.dot(m - pb, nb) * nb
        p = (1 - PHONG) * m + PHONG * 0.5 * (proj_a + proj_b)
        # (at most the sagitta of a circle through both ends turning as the normals do)
        off = p - m
        length = np.linalg.norm(pb - pa)
        limit = min(0.5 * length * math.tan(min(turn, math.radians(60)) / 4), 0.15 * length)
        if np.linalg.norm(off) > limit:
            off = off * (limit / np.linalg.norm(off))
        size = np.linalg.norm(off)
        if size > 1e-6:
            direction = Vector(off / size)
            hit = tree.ray_cast(Vector(m) + direction * 1e-4, direction, size + GAP)
            if hit[0] is not None:
                off = off * max(0.0, hit[3] - GAP) / size
        p = m + off
        mid[(a, b)] = len(V)
        V.append(p)
        w = W[a] + W[b]
        W.append(w / w.sum())
    if not mid:
        break
    NV, NN, NUV, NC, NM = [], [], [], [], []
    for t, ids in enumerate(TV):
        corners = [(ids[k], TN[t][k], TUV[t][k], TC[t][k]) for k in range(3)]
        splits = []
        for k in range(3):
            a, b = ids[k], ids[(k + 1) % 3]
            m = mid.get((min(a, b), max(a, b)))
            if m is None:
                splits.append(None)
            else:
                c0, c1 = corners[k], corners[(k + 1) % 3]
                splits.append((m, unit(c0[1] + c1[1]), 0.5 * (c0[2] + c1[2]), 0.5 * (c0[3] + c1[3])))
        # (rotate so the split edges come first)
        n_split = sum(s is not None for s in splits)
        if n_split == 0:
            pieces = [corners]
        elif n_split == 3:
            m0, m1, m2 = splits
            c0, c1, c2 = corners
            pieces = [[c0, m0, m2], [m0, c1, m1], [m2, m1, c2], [m0, m1, m2]]
        elif n_split == 1:
            k = next(i for i in range(3) if splits[i] is not None)
            c0, c1, c2 = corners[k], corners[(k + 1) % 3], corners[(k + 2) % 3]
            m0 = splits[k]
            pieces = [[c0, m0, c2], [m0, c1, c2]]
        else:
            k = next(i for i in range(3) if splits[i] is None)
            # the unsplit edge is c2-c0 after rotating: splits on c0-c1 and c1-c2
            k = (k + 1) % 3
            c0, c1, c2 = corners[k], corners[(k + 1) % 3], corners[(k + 2) % 3]
            m0, m1 = splits[k], splits[(k + 1) % 3]
            pieces = [[m0, c1, m1]]
            # the quad c0 m0 m1 c2 on its shorter diagonal
            if np.linalg.norm(V[c0[0]] - V[m1[0]]) <= np.linalg.norm(V[m0[0]] - V[c2[0]]):
                pieces += [[c0, m0, m1], [c0, m1, c2]]
            else:
                pieces += [[c0, m0, c2], [m0, m1, c2]]
        for piece in pieces:
            NV.append([c[0] for c in piece])
            NN.append([c[1] for c in piece])
            NUV.append([c[2] for c in piece])
            NC.append([c[3] for c in piece])
            NM.append(TM[t])
    TV, TN, TUV, TC, TM = NV, NN, NUV, NC, NM
    print(f"[hellcat-hd] level {level + 1}: {len(mid)} edges split, {len(TV)} triangles")

# --- into Blender, bevel ------------------------------------------------------------------------------
bpy.ops.wm.read_factory_settings(use_empty=True)
me = bpy.data.meshes.new("hellcat")
pos = np.array(V, np.float32)
n_tris = len(TV)
me.vertices.add(len(pos))
me.vertices.foreach_set("co", pos.ravel())
me.loops.add(3 * n_tris)
me.loops.foreach_set("vertex_index", np.array(TV, np.int32).ravel())
me.polygons.add(n_tris)
me.polygons.foreach_set("loop_start", np.arange(0, 3 * n_tris, 3, dtype=np.int32))
me.polygons.foreach_set("loop_total", np.full(n_tris, 3, np.int32))
me.polygons.foreach_set("material_index", np.array(TM, np.int32))
me.uv_layers.new(name="UV").data.foreach_set("uv", np.array(TUV, np.float32).ravel())
me.color_attributes.new("Col", "FLOAT_COLOR", "CORNER").data.foreach_set(
    "color", np.array(TC, np.float32).ravel())
me.update(calc_edges=True)
me.polygons.foreach_set("use_smooth", np.ones(n_tris, bool))
me.normals_split_custom_set([tuple(n) for n in np.array(TN).reshape(-1, 3)])
for name in tex_names:
    me.materials.append(bpy.data.materials.new(name))
obj = bpy.data.objects.new("hellcat", me)
bpy.context.scene.collection.objects.link(obj)
groups = [obj.vertex_groups.new(name=n) for n in joint_names]
for v, w in enumerate(W):
    for j in np.nonzero(w > 1e-4)[0]:
        groups[j].add([v], float(w[j]), "REPLACE")

# the edges to bevel: hard in the game (normals split across them) and folding enough
corner_n = np.array(TN).reshape(-1, 3, 3)
edge_users = {}
for t, ids in enumerate(TV):
    for k in range(3):
        a, b = ids[k], ids[(k + 1) % 3]
        edge_users.setdefault((min(a, b), max(a, b)), []).append((t, k))
face_n = unit(np.cross(pos[np.array(TV)[:, 1]] - pos[np.array(TV)[:, 0]],
                       pos[np.array(TV)[:, 2]] - pos[np.array(TV)[:, 0]]))
bevel_w = me.attributes.new("bevel_weight_edge", "FLOAT", "EDGE")
n_bevel = 0
for e in me.edges:
    users = edge_users.get(tuple(sorted(e.vertices)))
    if not users or len(users) != 2:
        continue
    (t1, k1), (t2, k2) = users
    fold = angle(face_n[t1], face_n[t2])
    va = e.vertices[0]
    n1 = corner_n[t1][TV[t1].index(va)]
    n2 = corner_n[t2][TV[t2].index(va)]
    if fold > BEVEL_ANGLE and angle(n1, n2) > math.radians(3):
        bevel_w.data[e.index].value = 1.0
        n_bevel += 1

bpy.context.view_layer.objects.active = obj
obj.select_set(True)
if BEVEL > 0 and n_bevel:
    bevel = obj.modifiers.new("bevel", "BEVEL")
    bevel.width = BEVEL
    bevel.segments = 2
    bevel.limit_method = "WEIGHT"
    bevel.use_clamp_overlap = True
    bevel.harden_normals = False
    bpy.ops.object.modifier_apply(modifier=bevel.name)
me = obj.data
bm = bmesh.new()
bm.from_mesh(me)
bmesh.ops.triangulate(bm, faces=bm.faces[:])
bm.to_mesh(me)
bm.free()
me.calc_loop_triangles()
print(f"[hellcat-hd] {n_bevel} edges bevelled; detailed: {len(me.vertices)} vertices, "
      f"{len(me.loop_triangles)} triangles")

# --- how far the surface moved --------------------------------------------------------------------
src_tree = BVHTree.FromPolygons([Vector(p) for p in game_pos], game_tris, all_triangles=True)
co = np.zeros(len(me.vertices) * 3, np.float32)
me.vertices.foreach_get("co", co)
co = co.reshape(-1, 3)
dist = np.array([src_tree.find_nearest(Vector(c))[3] or 0.0 for c in co])
print(f"[hellcat-hd] distance to the game's surface: mean {dist.mean() * 100:.2f} cm, "
      f"95% {np.percentile(dist, 95) * 100:.2f} cm, max {dist.max() * 100:.2f} cm")

# --- ambient occlusion into the vertex colours ----------------------------------------------------
vnorm = np.zeros(len(me.vertices) * 3, np.float32)
me.vertices.foreach_get("normal", vnorm)
vnorm = vnorm.reshape(-1, 3)
tree = BVHTree.FromPolygons([Vector(c) for c in co], [tuple(t.vertices) for t in me.loop_triangles],
                            all_triangles=True)
rng = random.Random(7)
dirs = []
while len(dirs) < 48:
    d = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)))
    if 0.05 < d.length <= 1:
        dirs.append(d.normalized())
occlusion = np.zeros(len(co), np.float32)
REACH = 0.6  # (contact shadows only: big triangles would get blotches from far occluders)
for i, (c, n) in enumerate(zip(co, vnorm)):
    n = Vector(n)
    origin = Vector(c) + n * 0.004
    hits = total = 0.0
    for d in dirs:
        cos = d.dot(n)
        if cos <= 0:
            d = -d
            cos = -cos
        hit = tree.ray_cast(origin, d, REACH)
        total += cos
        if hit[0] is not None:
            hits += cos * (1.0 - hit[3] / REACH) ** 0.5
    occlusion[i] = hits / total if total else 0.0
# (once smoothed over each vertex's neighbours: per-vertex noise reads as stains)
edge_v = np.zeros(len(me.edges) * 2, np.int32)
me.edges.foreach_get("vertices", edge_v)
edge_v = edge_v.reshape(-1, 2)
for _ in range(2):
    acc = occlusion.copy()
    cnt = np.ones(len(occlusion))
    np.add.at(acc, edge_v[:, 0], occlusion[edge_v[:, 1]])
    np.add.at(acc, edge_v[:, 1], occlusion[edge_v[:, 0]])
    np.add.at(cnt, edge_v[:, 0], 1)
    np.add.at(cnt, edge_v[:, 1], 1)
    occlusion = acc / cnt
shade = 1.0 - AO * occlusion
col_attr = me.color_attributes["Col"]
cols = np.zeros(len(me.loops) * 4, np.float32)
col_attr.data.foreach_get("color", cols)
cols = cols.reshape(-1, 4)
loop_vert = np.zeros(len(me.loops), np.int32)
me.loops.foreach_get("vertex_index", loop_vert)
cols[:, :3] *= shade[loop_vert, None]
col_attr.data.foreach_set("color", cols.ravel())
print(f"[hellcat-hd] ambient occlusion: mean shade {shade.mean():.3f}, darkest {shade.min():.3f}")

# --- gather the corners per texture ---------------------------------------------------------------
normals = np.zeros(len(me.loops) * 3, np.float32)
me.corner_normals.foreach_get("vector", normals)
normals = normals.reshape(-1, 3)
uvs = np.zeros(len(me.loops) * 2, np.float32)
me.uv_layers["UV"].data.foreach_get("uv", uvs)
uvs = uvs.reshape(-1, 2)
vweights = np.zeros((len(me.vertices), 4), np.float32)
vjoints = np.zeros((len(me.vertices), 4), np.uint8)
for v in me.vertices:
    best = sorted(((g.weight, g.group) for g in v.groups if g.weight > 1e-3), reverse=True)[:3]
    if not best:
        best = [(1.0, joint_names.index("main"))]
    total = sum(w for w, _ in best)
    for k, (w, g) in enumerate(best):
        vweights[v.index, k] = w / total
        vjoints[v.index, k] = g
tri_loops = np.zeros(len(me.loop_triangles) * 3, np.int32)
me.loop_triangles.foreach_get("loops", tri_loops)
tri_loops = tri_loops.reshape(-1, 3)
tri_mat = np.zeros(len(me.loop_triangles), np.int32)
me.loop_triangles.foreach_get("material_index", tri_mat)

if PREVIEW:
    scene = bpy.context.scene
    for slot, name in enumerate(tex_names):
        mat = me.materials[slot]
        mat.use_nodes = True
        nodes = mat.node_tree.nodes
        bsdf = next(n for n in nodes if n.type == "BSDF_PRINCIPLED")
        png = TEX_DIR / f"{name}.png"
        if png.exists():
            node = nodes.new("ShaderNodeTexImage")
            node.image = bpy.data.images.load(str(png))
            mat.node_tree.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "TEXTURE"
    scene.display.shading.show_cavity = False
    scene.render.resolution_x, scene.render.resolution_y = 1600, 1000
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    scene.camera = cam
    # (the glTF frame turned z up for the camera: the nose toward -y): from front-left, above
    obj.rotation_euler = (math.radians(90), 0, 0)
    cam.location = Vector(float(c) for c in option("--cam", "-8,-11,6").split(","))
    direction = Vector(float(c) for c in option("--look", "0,-0.5,0.3").split(",")) - cam.location
    cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    cam.data.lens = 40
    scene.render.filepath = PREVIEW
    bpy.ops.render.render(write_still=True)

# --- write the glb --------------------------------------------------------------------------------
out = {"asset": {"version": "2.0", "generator": "jak-enhanced hellcat_hd.py"}, "scene": 0,
       "scenes": [], "nodes": [], "meshes": [], "skins": [], "materials": [], "textures": [],
       "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
       "images": [], "accessors": [], "bufferViews": [], "buffers": []}
blob = bytearray()


def add_view(raw, target=None):
    while len(blob) % 4:
        blob.append(0)
    view = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(raw)}
    if target:
        view["target"] = target
    blob.extend(raw)
    out["bufferViews"].append(view)
    return len(out["bufferViews"]) - 1


def add_accessor(arr, gl_type, component, target=None, normalized=False, minmax=False):
    arr = np.ascontiguousarray(arr)
    acc = {"bufferView": add_view(arr.tobytes(), target), "componentType": component,
           "count": len(arr), "type": gl_type}
    if normalized:
        acc["normalized"] = True
    if minmax:
        acc["min"] = arr.min(0).tolist()
        acc["max"] = arr.max(0).tolist()
    out["accessors"].append(acc)
    return len(out["accessors"]) - 1


# the skeleton nodes and skin as in the rip, the mesh node first
old_nodes = gltf["nodes"]
mesh_node_idx = old_nodes.index(mesh_node)
order = [mesh_node_idx] + [i for i in range(len(old_nodes)) if i != mesh_node_idx]
remap = {old: new for new, old in enumerate(order)}
for old in order:
    node = json.loads(json.dumps(old_nodes[old]))
    if "children" in node:
        node["children"] = [remap[c] for c in node["children"]]
    if old == mesh_node_idx:
        node["mesh"] = 0
        node["skin"] = 0
    out["nodes"].append(node)
out["scenes"] = [{"nodes": [remap[n] for n in gltf["scenes"][gltf.get("scene", 0)]["nodes"]]}]
ibm = accessor(skin["inverseBindMatrices"]) if "inverseBindMatrices" in skin else None
new_skin = {"joints": [remap[j] for j in skin["joints"]]}
if "skeleton" in skin:
    new_skin["skeleton"] = remap[skin["skeleton"]]
if ibm is not None:
    new_skin["inverseBindMatrices"] = add_accessor(ibm.astype(np.float32), "MAT4", 5126)
out["skins"].append(new_skin)

primitives = []
total_tris = 0
for slot, name in enumerate(tex_names):
    tris = tri_loops[tri_mat == slot]
    if not len(tris):
        continue
    loops = tris.ravel()
    v = loop_vert[loops]
    # one vertex per distinct (position, normal, uv, colour) corner
    rows = np.hstack([v[:, None].astype(np.float32), np.round(normals[loops], 4), np.round(uvs[loops], 5),
                      np.round(cols[loops], 4)])
    uniq, first, inverse = np.unique(rows, axis=0, return_index=True, return_inverse=True)
    src = loops[first]
    sv = loop_vert[src]
    pos = co[sv]
    nrm = normals[src]
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-8)
    attrs = {
        "POSITION": add_accessor(pos.astype(np.float32), "VEC3", 5126, 34962, minmax=True),
        "NORMAL": add_accessor(nrm.astype(np.float32), "VEC3", 5126, 34962),
        "TEXCOORD_0": add_accessor(uvs[src].astype(np.float32), "VEC2", 5126, 34962),
        "COLOR_0": add_accessor(np.clip(cols[src], 0, 1).astype(np.float32), "VEC4", 5126, 34962),
        "JOINTS_0": add_accessor(vjoints[sv], "VEC4", 5121, 34962),
        "WEIGHTS_0": add_accessor(vweights[sv], "VEC4", 5126, 34962),
    }
    indices = add_accessor(inverse.astype(np.uint32).ravel(), "SCALAR", 5125, 34963)
    png = TEX_DIR / f"{name}.png"
    if not png.exists():
        raise SystemExit(f"{png} is missing: run hellcat_hd_textures.py")
    out["images"].append({"name": name, "mimeType": "image/png", "bufferView": add_view(png.read_bytes())})
    out["textures"].append({"name": name, "sampler": 0, "source": len(out["images"]) - 1})
    material = {"name": name, "doubleSided": True,
                "pbrMetallicRoughness": {"baseColorTexture": {"index": len(out["textures"]) - 1},
                                         "metallicFactor": 0.0}}
    if tex_blend[name]:
        material["alphaMode"] = "BLEND"
    else:
        material["alphaMode"] = "MASK"
        material["alphaCutoff"] = 0x26 / 255
    out["materials"].append(material)
    primitives.append({"attributes": attrs, "indices": indices, "material": len(out["materials"]) - 1,
                       "mode": 4})
    total_tris += len(tris)
out["meshes"].append({"name": mesh_node.get("name", "mesh"), "primitives": primitives})
while len(blob) % 4:
    blob.append(0)
out["buffers"].append({"byteLength": len(blob)})

js = json.dumps(out, separators=(",", ":")).encode()
js += b" " * (-len(js) % 4)
glb = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
glb += struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(blob), 0x004E4942) + bytes(blob)
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_bytes(glb)
print(f"[hellcat-hd] wrote {OUT}: {total_tris} triangles in {len(primitives)} draws "
      f"(the rip: {src_tris}), {len(out['images'])} textures, {len(glb) / 1e6:.1f} MB")
