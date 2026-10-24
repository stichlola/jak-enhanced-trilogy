"""Add the collision of the north part of the Jak 2 plaza (Blender, piazza_jak2.blend).

The plaza level only had the Jak 2 collision near the passages (COLL_jak2_palazzo, faces south of
GOAL z 880): the rest of the plaza is drawn but Jak falls through it. This imports the full ripped
collision (decompiler_out/jak2_pal/collision/collide-CPA.DGO-5-collide.obj, GOAL meters), keeps the
faces whose centre is below z 882 (a 2 m overlap with the existing piece, so the seam has no
gap) and stores them as COLL_jak2_palazzo_nord (collision only: glTF extra set_invisible) in
"Collisione nuova".

Run in the open .blend (Text Editor or the Blender MCP), then save.
"""
import os

import bmesh
import bpy

OBJ = os.path.join(os.path.dirname(bpy.data.filepath), "..", "..", "decompiler_out", "jak2_pal", "collision",
                   "collide-CPA.DGO-5-collide.obj")
NAME = "COLL_jak2_palazzo_nord"
SEAM_Z = 882.0  # GOAL z: the existing COLL_jak2_palazzo starts at ~880


def run():
    path = os.path.abspath(OBJ)
    old = bpy.data.objects.get(NAME)
    if old:
        bpy.data.objects.remove(old)
    before = set(bpy.data.objects)
    # OBJ is Y up: the importer turns (x, y, z) into Blender (x, -z, y), as for the other levels
    bpy.ops.wm.obj_import(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    ob = new[0]
    for o in new[1:]:
        bpy.data.objects.remove(o)
    ob.name = NAME
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    mw = ob.matrix_world
    kill = [f for f in bm.faces if -(mw @ f.calc_center_median()).y > SEAM_Z]
    bmesh.ops.delete(bm, geom=kill, context='FACES')
    bm.to_mesh(ob.data)
    bm.free()
    ob.data.materials.clear()
    for c in list(ob.users_collection):
        c.objects.unlink(ob)
    bpy.data.collections["Collisione nuova"].objects.link(ob)
    ob["set_invisible"] = 1  # like COLL_jak2_palazzo: default surface, not drawn
    ob.display_type = 'WIRE'
    print(f"[collision_north] {NAME}: {len(ob.data.polygons)} faces")
    return ob


if __name__ == "__main__":
    run()
