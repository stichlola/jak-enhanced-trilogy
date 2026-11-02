"""Bake the impostors of scenery models again from their .glb (after a change in how the views are laid out).

    blender -b --factory-startup -P scripts/enhanced/impostor_rebake.py -- OUT_DIR MODEL.glb [MODEL.glb ...]

Writes OUT_DIR/<model>_imp.png and <model>_nrm.png; a model whose pictures are there already is skipped,
so several of these can share a list and a run can be picked up again. Bakes as scenery_import.py does.
"""

import sys
import time
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

args = sys.argv[sys.argv.index("--") + 1:]
out_dir, models = Path(args[0]).resolve(), [Path(a).resolve() for a in args[1:]]
out_dir.mkdir(parents=True, exist_ok=True)

IMP_GRID, IMP_CELL = 8, 128


def hemi_oct_dir(u: float, v: float) -> Vector:
    """The view direction (game space: y up) of a cell's center (u, v in 0..1), as shrub_imp.vert:
    hemi-octahedral with the height squared, most views low (far things are seen nearly edge on)."""
    px, py = u * 2 - 1, v * 2 - 1
    x, z = (px + py) * 0.5, (px - py) * 0.5
    y = 1 - abs(x) - abs(z)
    return Vector((x, y * y, z)).normalized()


def impostor_setup() -> None:
    scene = bpy.context.scene
    cam = bpy.data.objects.new("imp_cam", bpy.data.cameras.new("imp_cam"))
    scene.collection.objects.link(cam)
    cam.data.type = "ORTHO"
    scene.camera = cam
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "FLAT"
    scene.display.shading.color_type = "TEXTURE"
    scene.view_settings.view_transform = "Standard"
    scene.render.film_transparent = True
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.resolution_x = scene.render.resolution_y = IMP_CELL
    scene.render.resolution_percentage = 100


def load(path: Path) -> bpy.types.Object:
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
    return obj


def bake_impostor(obj: bpy.types.Object, path: Path) -> None:
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
    for kind, out_path in (("VERTEX", path.with_name(path.stem.replace("_imp", "_nrm") + ".png")), ("TEXTURE", path)):
        out = bpy.data.images.new(out_path.stem, size, size, alpha=True)
        out.pixels[:] = atlases[kind].ravel()
        out.filepath_raw = str(out_path)
        out.file_format = "PNG"
        out.save()
        bpy.data.images.remove(out)


for model in models:
    path = out_dir / f"{model.stem}_imp.png"
    if path.exists():
        continue
    t0 = time.time()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    impostor_setup()
    bake_impostor(load(model), path)
    print(f"BAKED {model.stem} {time.time() - t0:.0f}s", flush=True)
