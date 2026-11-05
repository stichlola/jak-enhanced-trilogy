# Puts a ripped NPC (in a pose_editor.py .blend) in a plain unarmed idle to edit by hand: the first
# frame of its idle clip as a static pose (no action, so hand edits stick), arms hanging relaxed
# (elbows a little bent, hands at the front of the thighs), the hand-held gun hidden.
# blender -b <pose.blend> -P pose_rest_arms.py -- <idle-action>
import bpy, sys, math
from mathutils import Vector, Matrix

args = sys.argv[sys.argv.index("--") + 1:]
arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
pb = arm.pose.bones

# the clip's first frame, frozen
act = bpy.data.actions[args[0]]
arm.animation_data_create()
arm.animation_data.action = act
bpy.context.scene.frame_set(int(act.frame_range[0]))
frozen = {b.name: b.matrix.copy() for b in pb}
arm.animation_data.action = None
for b in pb:  # parents before children (bones are stored that way)
    b.matrix = frozen[b.name]
    bpy.context.view_layer.update()

# her frame: up, side (left), forward
up = Vector((0, 0, 1))
side = (pb["Lshoulder"].head - pb["Rshoulder"].head)
side.z = 0
side.normalize()
fwd = up.cross(side) if (pb["Lball"].head - pb["Lankle"].head).dot(up.cross(side)) > 0 else -up.cross(side)


def aim(name, direction):
    """Turn a bone about its head so it points along direction (armature space)."""
    b = pb[name]
    cur = (b.tail - b.head).normalized()
    rot = cur.rotation_difference(direction.normalized()).to_matrix().to_4x4()
    m = b.matrix.copy()
    head = m.translation.copy()
    m.translation = Vector()
    m = rot @ m
    m.translation = head
    b.matrix = m
    bpy.context.view_layer.update()


for s, out in (("L", side), ("R", -side)):
    aim(f"{s}shoulder", -up + 0.12 * out + 0.06 * fwd)   # upper arm down by the side
    aim(f"{s}elbow", -up + 0.38 * fwd - 0.10 * out)      # elbow a little bent, forearm forward/in
    aim(f"{s}hand", -up + 0.25 * fwd - 0.12 * out)       # hand following, relaxed

# the gun she holds: hidden (unarmed)
if "gun" in pb:
    pb["gun"].scale = (0.0001, 0.0001, 0.0001)
# plain sticks instead of the importer's sphere shapes
for b in pb:
    b.custom_shape = None
arm.data.display_type = "STICK"
bpy.ops.wm.save_mainfile()
