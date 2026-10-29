"""Import a user's model into the detailed scenery (EnhancedScenery), for the in-game editor.

    blender -b --factory-startup -P scripts/enhanced/scenery_import.py -- SRC.glb OUT_DIR NAME --max-faces N

Reads SRC.glb (or .gltf / .obj / .fbx), joins its meshes into one, stands it on the origin (its foot:
the middle of its bounding box, its lowest point at 0), keeping its size (meters). Past N faces it is
decimated down to N (the editor's budget per kind: grass small, rocks more). Writes, as the scenery's
own models (tools/blender/grass_variants.py in jak3-ue5 makes them the same way):
  OUT_DIR/NAME.glb        the model
  OUT_DIR/NAME_lod1.glb   its far version (a third of the faces)
  OUT_DIR/NAME_imp.png    its impostor: pictures from 8x8 directions over the upper hemisphere
  OUT_DIR/NAME_nrm.png    the impostor's normals
The last line printed is "RESULT {json}": faces in and out, materials, size, or "error".
"""

import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index("--") + 1:]
src, out_dir, name = Path(args[0]).resolve(), Path(args[1]).resolve(), args[2]
max_faces = int(args[args.index("--max-faces") + 1]) if "--max-faces" in args else 6000

LOD_RATIO = 0.33
IMP_GRID, IMP_CELL = 8, 128


def result(**kw) -> None:
    print("RESULT " + json.dumps(kw), flush=True)


def load(path: Path) -> bpy.types.Object:
    ext = path.suffix.lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=str(path))
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=str(path))
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=str(path))
    else:
        raise RuntimeError(f"unknown format {ext}")
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        raise RuntimeError("no mesh in the file")
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    # (the transforms into the vertices, parents dropped)
    bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for o in list(bpy.context.scene.objects):
        if o is not obj and o.type != "CAMERA":
            bpy.data.objects.remove(o)
    return obj


def stand_on_origin(obj: bpy.types.Object) -> Vector:
    vs = [v.co for v in obj.data.vertices]
    lo = Vector((min(v.x for v in vs), min(v.y for v in vs), min(v.z for v in vs)))
    hi = Vector((max(v.x for v in vs), max(v.y for v in vs), max(v.z for v in vs)))
    obj.data.transform(Matrix.Translation(Vector((-(lo.x + hi.x) / 2, -(lo.y + hi.y) / 2, -lo.z))))
    return hi - lo


def decimate(obj: bpy.types.Object, target: int) -> None:
    if len(obj.data.polygons) <= target:
        return
    bpy.context.view_layer.objects.active = obj
    dec = obj.modifiers.new("decimate", "DECIMATE")
    dec.ratio = target / len(obj.data.polygons)
    bpy.ops.object.modifier_apply(modifier=dec.name)


def export(obj: bpy.types.Object, path: Path) -> None:
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.gltf(filepath=str(path), use_selection=True, export_format="GLB",
                              export_apply=True, export_yup=True)


def hemi_oct_dir(u: float, v: float) -> Vector:
    """The view direction (game space: y up) of a hemi-octahedral grid cell's center (u, v in 0..1),
    as the game picks it (EnhancedScenery, shrub_imp.vert)."""
    px, py = u * 2 - 1, v * 2 - 1
    x, z = (px + py) * 0.5, (px - py) * 0.5
    return Vector((x, 1 - abs(x) - abs(z), z)).normalized()


def impostor_setup() -> None:
    scene = bpy.context.scene
    cam = bpy.data.objects.new("imp_cam", bpy.data.cameras.new("imp_cam"))
    scene.collection.objects.link(cam)
    cam.data.type = "ORTHO"
    scene.camera = cam
    try:
        scene.render.engine = "BLENDER_WORKBENCH"
    except TypeError:
        pass
    scene.display.shading.light = "FLAT"
    scene.display.shading.color_type = "TEXTURE"
    scene.view_settings.view_transform = "Standard"
    scene.render.film_transparent = True
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.resolution_x = scene.render.resolution_y = IMP_CELL
    scene.render.resolution_percentage = 100


def bake_impostor(obj: bpy.types.Object, path: Path) -> None:
    """Pictures of obj from IMP_GRID x IMP_GRID directions, one atlas, and its normals (as
    grass_variants.py's bake_impostor: the game reads both the same way)."""
    scene = bpy.context.scene
    corners = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    lo = Vector((min(c.x for c in corners), min(c.y for c in corners), min(c.z for c in corners)))
    hi = Vector((max(c.x for c in corners), max(c.y for c in corners), max(c.z for c in corners)))
    center, r = (lo + hi) / 2, (hi - lo).length / 2
    cam = scene.camera
    cam.data.ortho_scale = 2 * r
    cam.data.clip_end = 10 * r
    mesh = obj.data
    nrm = mesh.color_attributes.new("imp_normal", "BYTE_COLOR", "CORNER")
    cn = mesh.corner_normals
    for k, c in enumerate(nrm.data):
        n = cn[k].vector
        c.color_srgb = (n.x * 0.5 + 0.5, n.z * 0.5 + 0.5, -n.y * 0.5 + 0.5, 1.0)
    mesh.color_attributes.active_color = nrm
    mesh.color_attributes.render_color_index = mesh.color_attributes.find("imp_normal")
    size = IMP_CELL * IMP_GRID
    atlases = {"TEXTURE": np.zeros((size, size, 4), dtype=np.float32),
               "VERTEX": np.zeros((size, size, 4), dtype=np.float32)}
    tmp = path.with_suffix(".cell.png")
    b = lambda g: Vector((g.x, -g.z, g.y))  # game (x, y, z) -> Blender (x, -z, y)
    for j in range(IMP_GRID):
        for i in range(IMP_GRID):
            d = hemi_oct_dir((i + 0.5) / IMP_GRID, (j + 0.5) / IMP_GRID)
            f = -d
            right = f.cross(Vector((0, 1, 0))).normalized()
            up = right.cross(f)
            cam.matrix_world = Matrix.Translation(center + b(d) * 4 * r) @ \
                Matrix((b(right), b(up), -b(f))).transposed().to_4x4()
            for kind, atlas in atlases.items():
                scene.display.shading.color_type = kind
                scene.render.filepath = str(tmp)
                bpy.ops.render.render(write_still=True)
                img = bpy.data.images.load(str(tmp))
                px = np.array(img.pixels[:], dtype=np.float32).reshape(IMP_CELL, IMP_CELL, 4)
                atlas[j * IMP_CELL:(j + 1) * IMP_CELL, i * IMP_CELL:(i + 1) * IMP_CELL] = px
                bpy.data.images.remove(img)
    tmp.unlink()
    scene.display.shading.color_type = "TEXTURE"
    mesh.color_attributes.remove(mesh.color_attributes["imp_normal"])
    for kind, out_path in (("TEXTURE", path), ("VERTEX", path.with_name(path.stem.replace("_imp", "_nrm") + ".png"))):
        out = bpy.data.images.new(out_path.stem, size, size, alpha=True)
        out.pixels[:] = atlases[kind].ravel()
        out.filepath_raw = str(out_path)
        out.file_format = "PNG"
        out.save()
        bpy.data.images.remove(out)


try:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    obj = load(src)
    obj.name = name
    faces_in = len(obj.data.polygons)
    materials = len([m for m in obj.data.materials if m])
    dims = stand_on_origin(obj)
    decimate(obj, max_faces)
    faces_out = len(obj.data.polygons)
    export(obj, out_dir / f"{name}.glb")
    impostor_setup()
    bake_impostor(obj, out_dir / f"{name}_imp.png")
    far = obj.copy()
    far.data = obj.data.copy()
    bpy.context.scene.collection.objects.link(far)
    decimate(far, max(int(faces_out * LOD_RATIO), min(faces_out, 300)))
    export(far, out_dir / f"{name}_lod1.glb")
    result(ok=True, faces_in=faces_in, faces=faces_out, lod_faces=len(far.data.polygons), materials=materials,
           size=[round(dims.x, 2), round(dims.z, 2), round(dims.y, 2)])
except Exception as e:  # (the editor shows it)
    result(ok=False, error=str(e))
