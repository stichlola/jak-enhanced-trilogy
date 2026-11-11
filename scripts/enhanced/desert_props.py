"""Clean and size one of the desert props (Hunyuan3D models) for the placed scenery.

    blender -b --factory-startup -P scripts/enhanced/desert_props.py -- SRC.glb OUT.glb KIND [--col COL.glb] [--preview PNG]

KIND: tree (4 m tall), shrub (1 m tall), boulder (2.5 m across), cart (2 m long). Steps:
  - mesh hygiene (as the mesh-hygiene skill): joined, triangulated, doubles merged (0.1 mm),
    degenerate triangles dissolved, loose vertices/edges and tiny islands removed, normals
    recomputed; the numbers before and after are printed (HYGIENE lines);
  - scaled to KIND's size, standing on the origin (its foot: the middle of its bottom);
  - the base color texture only (the scenery shader reads no other map), downscaled
    (tree/boulder 2048, shrub/cart 1024);
  - the tree and the shrub get a wind weight per vertex in COLOR_0 (gray and alpha alike), read by
    EnhancedScenery / shrub_inst.vert: leaves (yellow-green texels) 0.6 + 0.4 * the height above
    35% of the model (0 at the trunk's foot); the shrub by its height only.
Then scripts/enhanced/scenery_import.py makes its far version and impostor.
"""

import colorsys
import json
import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index("--") + 1:]
src, out, kind = Path(args[0]).resolve(), Path(args[1]).resolve(), args[2]
preview = Path(args[args.index("--preview") + 1]) if "--preview" in args else None

SIZE = {"tree": ("height", 4.0), "shrub": ("height", 1.0), "boulder": ("max", 2.5), "cart": ("length", 2.0)}
TEX = {"tree": 2048, "boulder": 2048, "shrub": 1024, "cart": 1024}
FACES = {"tree": 12000, "shrub": 7000, "boulder": 6000, "cart": 8000}


def load(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(path))
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for o in list(bpy.context.scene.objects):
        if o is not obj:
            bpy.data.objects.remove(o)
    return obj


def islands(bm):
    seen, out = set(), []
    for v in bm.verts:
        if v.index in seen:
            continue
        stack, isl = [v], []
        seen.add(v.index)
        while stack:
            a = stack.pop()
            isl.append(a)
            for e in a.link_edges:
                b = e.other_vert(a)
                if b.index not in seen:
                    seen.add(b.index)
                    stack.append(b)
        out.append(isl)
    return out


def stats(obj):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.verts.index_update()
    keys = {}
    for v in bm.verts:
        k = (round(v.co.x, 4), round(v.co.y, 4), round(v.co.z, 4))
        keys[k] = keys.get(k, 0) + 1
    dup = sum(n - 1 for n in keys.values() if n > 1)
    degen = 0
    for f in bm.faces:
        a = f.calc_area()
        longest = max(e.calc_length() for e in f.edges)
        if a < 1e-10 or (longest > 0 and 2 * a / longest < 1e-5):
            degen += 1
    nonman = sum(1 for e in bm.edges if not e.is_manifold)
    size = max(obj.dimensions)
    small = sum(1 for i in islands(bm) if len(i) < 30)
    r = {"faces": len(bm.faces), "dup_verts": dup, "degenerate": degen, "non_manifold": nonman,
         "small_islands": small, "volume_m3": round(abs(bm.calc_volume()), 4)}
    bm.free()
    return r


def clean(obj):
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=0.0001)
    for _ in range(3):
        bmesh.ops.dissolve_degenerate(bm, edges=bm.edges[:], dist=0.00001)
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context="VERTS")
    loose_e = [e for e in bm.edges if not e.link_faces]
    bmesh.ops.delete(bm, geom=loose_e, context="EDGES")
    # (flat triangles left: collapse their shortest edge)
    bm.verts.index_update()
    flat = []
    for f in bm.faces:
        if not f.is_valid:
            continue
        a = f.calc_area()
        longest = max(e.calc_length() for e in f.edges)
        if a < 1e-10 or (longest > 0 and 2 * a / longest < 1e-5):
            flat.append(min(f.edges, key=lambda e: e.calc_length()))
    flat = list({e for e in flat if e.is_valid})
    if flat:
        bmesh.ops.collapse(bm, edges=flat, uvs=True)
    # tiny loose specks: fewer than 30 vertices and under 2% of the model's size
    bm.verts.ensure_lookup_table()
    bm.verts.index_update()
    lo = Vector((min(v.co.x for v in bm.verts), min(v.co.y for v in bm.verts), min(v.co.z for v in bm.verts)))
    hi = Vector((max(v.co.x for v in bm.verts), max(v.co.y for v in bm.verts), max(v.co.z for v in bm.verts)))
    diag = (hi - lo).length
    specks = []
    for isl in islands(bm):
        if len(isl) >= 30:
            continue
        l = Vector((min(v.co.x for v in isl), min(v.co.y for v in isl), min(v.co.z for v in isl)))
        h = Vector((max(v.co.x for v in isl), max(v.co.y for v in isl), max(v.co.z for v in isl)))
        if (h - l).length < 0.02 * diag:
            specks += isl
    removed = 0
    if specks:
        removed = len({v.index for v in specks})
        bmesh.ops.delete(bm, geom=list({v for v in specks}), context="VERTS")
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    bm.to_mesh(me)
    bm.free()
    if me.has_custom_normals:
        bpy.context.view_layer.objects.active = obj
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    for p in me.polygons:
        p.use_smooth = True
    try:
        me.set_sharp_from_angle(angle=math.radians(50))
    except Exception:
        pass
    return removed


def decimate(obj, target):
    if len(obj.data.polygons) <= target:
        return
    bpy.context.view_layer.objects.active = obj
    dec = obj.modifiers.new("decimate", "DECIMATE")
    dec.ratio = target / len(obj.data.polygons)
    bpy.ops.object.modifier_apply(modifier=dec.name)


def base_image(obj):
    for mat in obj.data.materials:
        if not mat or not mat.node_tree:
            continue
        bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
        if bsdf and bsdf.inputs["Base Color"].is_linked:
            n = bsdf.inputs["Base Color"].links[0].from_node
            if n.type == "TEX_IMAGE" and n.image:
                return n.image
    imgs = [n.image for m in obj.data.materials if m and m.node_tree for n in m.node_tree.nodes
            if n.type == "TEX_IMAGE" and n.image and "normal" not in n.image.name and "metal" not in n.image.name]
    return imgs[0] if imgs else None


def fit(obj):
    me = obj.data
    co = np.array([v.co[:] for v in me.vertices])
    lo, hi = co.min(0), co.max(0)
    ext = hi - lo
    how, size = SIZE[kind]
    have = {"height": ext[2], "max": ext.max(), "length": max(ext[0], ext[1])}[how]
    k = size / have
    me.transform(Matrix.Translation(Vector((-(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -lo[2]))))
    me.transform(Matrix.Scale(k, 4))
    return k, [round(float(e * k), 2) for e in ext]


def wind(obj, img):
    me = obj.data
    co = np.array([v.co[:] for v in me.vertices])
    h = co[:, 2] / max(co[:, 2].max(), 1e-6)
    above = np.clip((h - 0.35) / 0.65, 0, 1)
    leaf = np.zeros(len(me.vertices))
    if kind == "tree" and img is not None:
        w, hh = img.size
        px = np.array(img.pixels[:], dtype=np.float32).reshape(hh, w, img.channels)
        uv = me.uv_layers.active.data
        sums = np.zeros(len(me.vertices))
        cnt = np.zeros(len(me.vertices))
        for loop in me.loops:
            u, v = uv[loop.index].uv
            x = int((u % 1.0) * (w - 1))
            y = int((v % 1.0) * (hh - 1))
            r, g, b = px[y, x, :3]
            hue, s, val = colorsys.rgb_to_hsv(float(r), float(g), float(b))
            # yellow-green leaves: hue 50..170 degrees, some saturation
            sums[loop.vertex_index] += 1.0 if (50 / 360 <= hue <= 170 / 360 and s > 0.18 and val > 0.08) else 0.0
            cnt[loop.vertex_index] += 1
        leaf = sums / np.maximum(cnt, 1)
        weight = leaf * 0.6 + 0.4 * above
    else:
        weight = np.clip(h, 0, 1) ** 1.5
    for a in [a for a in me.color_attributes]:
        me.color_attributes.remove(a)
    attr = me.color_attributes.new("wind", "BYTE_COLOR", "POINT")
    for i, c in enumerate(attr.data):
        c.color = (float(weight[i]),) * 4
    me.color_attributes.active_color = attr
    return float(weight.mean()), float((leaf > 0.5).mean())


def simple_material(obj, img):
    side = TEX[kind]
    if img is not None and max(img.size) > side:
        img.scale(side, side)
    mat = bpy.data.materials.new(f"{kind}_mat")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Roughness"].default_value = 0.9
    if img is not None:
        img.pack()
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = img
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    obj.data.materials.clear()
    obj.data.materials.append(mat)


def export(obj, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    kw = dict(filepath=str(path), use_selection=True, export_format="GLB", export_apply=True, export_yup=True,
              export_image_format="JPEG", export_jpeg_quality=90)
    for extra in ({"export_vertex_color": "ACTIVE"}, {"export_colors": True}, {}):
        try:
            bpy.ops.export_scene.gltf(**kw, **extra)
            return
        except TypeError:
            continue


def collision_proxy(obj, path):
    """A simple stand-in for its collision (EnhancedScenery reads <model>_col.glb): the tree's trunk
    as a 12-sided cylinder 2.4 m tall, the boulder's convex hull (~80 faces), the cart's box."""
    co = np.array([v.co[:] for v in obj.data.vertices])
    hgt = co[:, 2].max()
    bm = bmesh.new()
    if kind == "tree":
        low = co[(co[:, 2] > 0.05 * hgt) & (co[:, 2] < 0.3 * hgt)]
        c = low[:, :2].mean(0)
        r = float(np.percentile(np.linalg.norm(low[:, :2] - c, axis=1), 70))
        bmesh.ops.create_cone(bm, cap_ends=True, segments=12, radius1=r, radius2=r * 0.8, depth=2.4,
                              matrix=Matrix.Translation(Vector((c[0], c[1], 1.2))))
    elif kind == "boulder":
        for p in co:
            bm.verts.new(p)
        bmesh.ops.convex_hull(bm, input=bm.verts[:])
        bm.verts.ensure_lookup_table()
        loose = [v for v in bm.verts if not v.link_faces]
        bmesh.ops.delete(bm, geom=loose, context="VERTS")
    elif kind == "cart":
        lo, hi = co.min(0), co.max(0)
        bmesh.ops.create_cube(bm, size=1.0, matrix=Matrix.Translation(Vector((lo + hi) / 2)) @
                              Matrix.Diagonal(Vector((*(hi - lo) * 0.92, 1.0))).to_4x4())
    else:
        bm.free()
        return 0
    me = bpy.data.meshes.new("col")
    bm.to_mesh(me)
    bm.free()
    col = bpy.data.objects.new(path.stem, me)
    bpy.context.scene.collection.objects.link(col)
    if kind == "boulder":
        bpy.context.view_layer.objects.active = col
        dec = col.modifiers.new("decimate", "DECIMATE")
        dec.ratio = min(1.0, 80 / max(len(me.polygons), 1))
        bpy.ops.object.modifier_apply(modifier=dec.name)
    faces = len(col.data.polygons)
    export(col, path)
    bpy.data.objects.remove(col)
    return faces


def render_preview(obj, path):
    scene = bpy.context.scene
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    scene.camera = cam
    d = obj.dimensions
    r = max(d) * 0.6
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = max(d) * 1.2
    cam.location = (r * 3, -r * 3, d.z / 2 + r * 1.2)
    cam.rotation_euler = (math.radians(72), 0, math.radians(45))
    try:
        scene.render.engine = "BLENDER_WORKBENCH"
    except TypeError:
        pass
    scene.display.shading.color_type = "TEXTURE"
    scene.render.resolution_x = scene.render.resolution_y = 384
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(cam)


obj = load(src)
img = base_image(obj)
before = stats(obj)
specks = clean(obj)
decimate(obj, FACES[kind])
k, dims = fit(obj)
after = stats(obj)
wmean, leaves = wind(obj, img) if kind in ("tree", "shrub") else (0.0, 0.0)
simple_material(obj, img)
export(obj, out)
col_faces = collision_proxy(obj, Path(args[args.index("--col") + 1])) if "--col" in args else 0
if preview:
    render_preview(obj, preview)
print("HYGIENE " + json.dumps({"kind": kind, "before": before, "after": after, "specks_removed": specks,
                                "scale": round(k, 3), "dims_m": dims, "wind_mean": round(wmean, 3),
                                "leaf_share": round(leaves, 3), "collision_faces": col_faces}), flush=True)
