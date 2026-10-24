"""Export the Jak 2 plaza custom level (piazza.glb) from its working .blend, headless.

   blender -b local/blender/piazza_jak2.blend -P scripts/enhanced/piazza_export.py [-- <out.glb>]

Exports the collections "Jak2 palazzo" (plaza background), "Passaggio" (ramps and rubble) and
"Collisione nuova" (collision-only meshes, glTF extra set_invisible) minus objects tagged
export_skip. Materials are made opaque first (the ripped plaza materials link the texture alpha,
the custom level builder then treats them as see-through and draws nothing) except real cutouts.
In our own collision meshes and ramps (not the ripped Jak 2 collision or the rubble shells cut from
the game), floors facing down with nothing of their own mesh above them are flipped: the game's
collision is one-sided, Jak falls through them (the port ramp did). Near the passages the one-sided
Jak 2 plaza faces get a back side.
The .blend is not saved: the user's live copy stays as it is.
"""
import math
import os
import sys

import bpy
import bmesh
from mathutils import Vector
from mathutils.bvhtree import BVHTree

COLLECTIONS = ("Jak2 palazzo", "Passaggio", "Collisione nuova")
CUTOUT_WORDS = ("tree", "vine", "flower", "leaf", "leaves", "grass", "fence", "rail", "plant",
                "canopy", "shrub", "bush", "glass", "window")
DEFAULT_OUT = os.path.join(os.path.dirname(__file__), "..", "..", "custom_assets", "jak3", "levels",
                           "piazza", "piazza.glb")

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
out = os.path.abspath(argv[0] if argv else DEFAULT_OUT)

objects = [o for c in COLLECTIONS for o in bpy.data.collections[c].all_objects
           if o.type == "MESH" and not o.get("export_skip")]

opaque = 0
for mat in {m for o in objects for m in o.data.materials if m and m.node_tree}:
    if any(w in mat.name.lower() for w in CUTOUT_WORDS):
        continue
    bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf:
        for link in list(bsdf.inputs["Alpha"].links):
            mat.node_tree.links.remove(link)
            opaque += 1
        bsdf.inputs["Alpha"].default_value = 1.0

flipped = 0
for o in objects:
    if not (o.name.startswith("COLL_") or o.name.startswith("Passaggio rampa")) or \
            o.name == "COLL_jak2_palazzo":
        continue
    nm = o.matrix_world.to_3x3().inverted().transposed()
    tree = BVHTree.FromObject(o, bpy.context.evaluated_depsgraph_get())
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bm.faces.ensure_lookup_table()
    down = []
    for f in bm.faces:
        if (nm @ f.normal).normalized().z < -0.7:
            # object space ray straight up (world +Z) from just above the face
            up = (o.matrix_world.to_3x3().inverted() @ Vector((0, 0, 1))).normalized()
            hit = tree.ray_cast(f.calc_center_median() + up * 0.01, up)
            if hit[0] is None:
                down.append(f)
    if down:
        bmesh.ops.reverse_faces(bm, faces=down)
        bm.to_mesh(o.data)
        flipped += len(down)
        print(f"[piazza_export] {o.name}: {len(down)} floor faces were facing down, flipped")
    bm.free()


def near_passage(x, y, z, margin):
    """Is the GOAL-meter point within margin of a passage (the cut volumes of Shader.cpp)?"""
    if 42 - margin < x < 92 + margin and 950 - margin < z < 968 + margin and y < 60:
        return True
    ax, az, bx, bz = 229.0, 1146.0, 200.0, 1206.0
    t = max(0.0, min(1.0, ((x - ax) * (bx - ax) + (z - az) * (bz - az)) /
                     ((bx - ax) ** 2 + (bz - az) ** 2)))
    return math.hypot(x - (ax + (bx - ax) * t), z - (az + (bz - az) * t)) < 6.5 + margin and y < 40


# The Jak 2 plaza was only ever seen from inside: its faces are one-sided and from the new passages
# the backs of pillars and walls show as missing triangles. Near the passages, add a flipped copy
# of every face (drawing only: these objects take no collision).
doubled = 0
for o in objects:
    if o.users_collection[0].name != "Jak2 palazzo":
        continue
    mw = o.matrix_world
    bm = bmesh.new()
    bm.from_mesh(o.data)
    near = []
    for f in bm.faces:
        c = mw @ f.calc_center_median()
        if near_passage(c.x, c.z, -c.y, 25.0):
            near.append(f)
    if near:
        copy = bmesh.ops.duplicate(bm, geom=near)["geom"]
        bmesh.ops.reverse_faces(bm, faces=[g for g in copy if isinstance(g, bmesh.types.BMFace)])
        bm.to_mesh(o.data)
        doubled += len(near)
    bm.free()

for o in bpy.context.view_layer.objects:
    o.select_set(False)
for o in objects:
    o.hide_viewport = False
    o.hide_set(False)
    o.select_set(True)

bpy.ops.export_scene.gltf(filepath=out, use_selection=True, export_extras=True, export_yup=True,
                          export_apply=True, export_vertex_color="ACTIVE", export_format="GLB")
faces = sum(len(o.data.polygons) for o in objects)
print(f"[piazza_export] {len(objects)} objects, {faces} faces, {opaque} alpha links cut, "
      f"{flipped} faces flipped, {doubled} doubled -> {out} ({os.path.getsize(out) // 1024} KB)")
