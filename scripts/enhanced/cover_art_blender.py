"""Blender side of cover_art.py: renders one of the game's characters, posed, on a transparent background.

    blender -b --factory-startup -P scripts/enhanced/cover_art_blender.py -- <job.json>

job.json: {"out": png, "size": [w, h], "parts": [{"glb", "action", "frame" (0..1 of the action),
"attach": bone (optional: parented to that bone of the first part's armature)}], "yaw": degrees
(the camera around the character, 0 = straight in front), "pitch": degrees, "frame_to": "body" or
"head" (what fills the picture), "zoom": 1.0}.
The characters face -Y once imported (GOAL's +Z forward, glTF's Y up); the camera looks at them
from the front, lit with a warm key light, a cool fill and two rim lights from behind.
"""
import json
import math
import sys

import bpy
from mathutils import Euler, Matrix, Vector

job = json.load(open(sys.argv[sys.argv.index("--") + 1]))
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
FRAME = 1000

armatures = []
meshes = []
for part in job["parts"]:
    before = set(bpy.data.objects)
    # (bones as the glTF joints: a prop on a bone then sits in the joint's own frame, as in the game)
    bpy.ops.import_scene.gltf(filepath=part["glb"], bone_heuristic="BLENDER")
    new = [o for o in bpy.data.objects if o not in before]
    # (not the ripper's debug sphere)
    for o in [o for o in new if o.type == "MESH" and o.name.startswith("Icosphere")]:
        new.remove(o)
        bpy.data.objects.remove(o)
    arm = next((o for o in new if o.type == "ARMATURE"), None)
    if arm and part.get("action"):
        # every action played so its chosen frame lands on the scene's frame FRAME (an NLA strip
        # each: the parts' actions have different lengths)
        act = bpy.data.actions[part["action"]]
        arm.animation_data_create()
        arm.animation_data.action = None
        lo, hi = act.frame_range
        chosen = lo + (hi - lo) * part.get("frame", 0.0)
        strip = arm.animation_data.nla_tracks.new().strips.new(act.name, int(FRAME - (chosen - lo)), act)
        if hasattr(strip, "action_slot") and act.slots:
            strip.action_slot = act.slots[0]
    if part.get("attach") and armatures:
        # a prop in the hand: its root follows the bone (the gun bone carries the grip)
        target = armatures[0]
        roots = [o for o in new if o.parent is None]
        for o in roots:
            c = o.constraints.new("CHILD_OF")
            c.target = target
            c.subtarget = part["attach"]
            c.set_inverse_pending = False
            # (the glTF importer turns each glb's Y up to Blender's Z up: undone for the prop, which
            # takes the joint's frame directly)
            c.inverse_matrix = Euler([math.radians(a) for a in part.get("attach_rot", (-90, 0, 0))]).to_matrix().to_4x4()
    if arm:
        armatures.append(arm)
    meshes += [o for o in new if o.type == "MESH"]

scene.frame_set(FRAME)
bpy.context.view_layer.update()
# Daxter rides Jak's shoulder as in sidekick.gc (and jak3-ue5's PlaceDaxter): his pelvis ("main")
# held on Jak's LshoulderPad joint, a little out and up, turned with Jak's body, at 0.6 of his
# ripped size (the ripper drops the align/prejoint channels that seat and scale him in the game)
for part, arm in zip([p for p in job["parts"] if p.get("action")], armatures):
    if part.get("ride") and arm is not armatures[0]:
        jak = armatures[0]
        pad = (jak.matrix_world @ jak.pose.bones[part["ride"]].matrix).to_translation()
        # the seat: x out to Jak's left (+X, he faces -Y), y back, z up (meters)
        seat = pad + Vector(part.get("seat", (0.16, 0.06, 0.10)))
        scale = part.get("scale", 0.6)
        arm.matrix_world = jak.matrix_world.to_3x3().to_4x4() @ Matrix.Scale(scale, 4)
        bpy.context.view_layer.update()
        main = (arm.matrix_world @ arm.pose.bones["main"].matrix).to_translation()
        arm.matrix_world = Matrix.Translation(seat - main) @ arm.matrix_world
bpy.context.view_layer.update()

# smoothed (the job's "smooth"): the shared corners welded, smooth shading, subdivided twice, so the
# low-poly PS2 models read as modern CGI
if job.get("smooth"):
    import bmesh
    for o in meshes:
        bm = bmesh.new()
        bm.from_mesh(o.data)
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0004)
        bm.to_mesh(o.data)
        bm.free()
        for poly in o.data.polygons:
            poly.use_smooth = True
        mod = o.modifiers.new("smooth", "SUBSURF")
        mod.levels = mod.render_levels = 2
        mod.boundary_smooth = "PRESERVE_CORNERS"
    bpy.context.view_layer.update()

# the materials: the ripper's vertex colors are the game's baked light (0.5 = as the texture);
# multiply them in, doubled, so the faces keep the game's shading
for o in meshes:
    if not o.data.color_attributes:
        continue
    attr = o.data.color_attributes[0].name
    for slot in o.material_slots:
        m = slot.material
        if not m or not m.use_nodes or m.get("_vc"):
            continue
        m["_vc"] = True
        nt = m.node_tree
        bsdf = next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)
        if not bsdf:
            continue
        base = bsdf.inputs["Base Color"]
        vc = nt.nodes.new("ShaderNodeVertexColor")
        vc.layer_name = attr
        mul = nt.nodes.new("ShaderNodeMix")
        mul.data_type = "RGBA"
        mul.blend_type = "MULTIPLY"
        mul.inputs["Factor"].default_value = 1.0
        if base.links:
            nt.links.new(base.links[0].from_socket, mul.inputs["A"])
        else:
            mul.inputs["A"].default_value = base.default_value
        rgb = nt.nodes.new("ShaderNodeMix")
        rgb.data_type = "RGBA"
        rgb.blend_type = "MULTIPLY"
        rgb.inputs["Factor"].default_value = 1.0
        nt.links.new(vc.outputs["Color"], rgb.inputs["A"])
        rgb.inputs["B"].default_value = (job.get("vc_gain", 2.0),) * 3 + (1.0,)
        nt.links.new(rgb.outputs["Result"], mul.inputs["B"])
        nt.links.new(mul.outputs["Result"], base)
        bsdf.inputs["Roughness"].default_value = 0.6
        if "Specular IOR Level" in bsdf.inputs:
            bsdf.inputs["Specular IOR Level"].default_value = 0.3

# what the camera frames: the whole body, or the head (neck bone up)
def world_bbox(objs):
    pts = []
    deps = bpy.context.evaluated_depsgraph_get()
    for o in objs:
        ev = o.evaluated_get(deps)
        me = ev.to_mesh()
        pts += [ev.matrix_world @ v.co for v in me.vertices]
        ev.to_mesh_clear()
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    return lo, hi

lo, hi = world_bbox(meshes)
if job.get("frame_to") == "head" and armatures:
    arm = armatures[0]
    neck = arm.matrix_world @ arm.pose.bones["neckA"].head
    lo = Vector((lo.x, lo.y, neck.z - 0.15))
center = (lo + hi) / 2
height = hi.z - lo.z
w, h = job["size"]
cam_data = bpy.data.cameras.new("cam")
cam_data.lens = 70
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
yaw = math.radians(job.get("yaw", 25))
pitch = math.radians(job.get("pitch", 5))
# the distance that fits the height (vertical fov from the sensor, fit "auto" = the longer side)
aspect = w / h
# (sensor fit "auto": the sensor width spans the longer side of the picture)
half = cam_data.sensor_width / 2 / cam_data.lens
fov_v = 2 * math.atan(half if aspect < 1 else half / aspect)
dist = (height / 2) / math.tan(fov_v / 2) / job.get("zoom", 0.88)
direction = Vector((math.sin(yaw) * math.cos(pitch), -math.cos(yaw) * math.cos(pitch), math.sin(pitch)))
cam.location = center + direction * dist
cam.rotation_euler = (-direction).to_track_quat("-Z", "Y").to_euler()

def light(name, kind, energy, color, rot, size=None):
    d = bpy.data.lights.new(name, kind)
    d.energy = energy
    d.color = color
    if size is not None and hasattr(d, "angle"):
        d.angle = size
    o = bpy.data.objects.new(name, d)
    o.rotation_euler = [math.radians(a) for a in rot]
    scene.collection.objects.link(o)

key = job.get("key", (1.0, 0.93, 0.82))
rim = job.get("rim", (0.6, 0.8, 1.0))
light("key", "SUN", 4.0, key, (55, 0, -35), 0.3)
light("fill", "SUN", 1.2, (0.75, 0.82, 1.0), (70, 0, 50), 0.6)
light("rimL", "SUN", 5.0, rim, (75, 0, 150), 0.1)
light("rimR", "SUN", 4.0, job.get("rim2", rim), (75, 0, -150), 0.1)
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.35, 0.37, 0.42, 1)
world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
scene.world = world

for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
    try:
        scene.render.engine = engine
        break
    except TypeError:
        pass
scene.render.film_transparent = True
scene.render.resolution_x, scene.render.resolution_y = w, h
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"
scene.view_settings.view_transform = "Standard"
scene.render.filepath = job["out"]
bpy.ops.render.render(write_still=True)
print("RENDERED", job["out"])
