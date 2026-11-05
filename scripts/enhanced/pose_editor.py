# Makes a .blend of a character's ripped model, ready to pose by hand (pose mode, bones in front);
# open it in Blender, pose, save: the pose is read back from it and put into the game.
# blender -b -P pose_editor.py -- <model.glb> <out.blend> [action-name]   then   blender <out.blend>
# (run headless: in the UI the startup file loads over what the script imported)
import bpy, sys

args = sys.argv[sys.argv.index("--") + 1:]
src, out = args[0], args[1]
action = args[2] if len(args) > 2 else None

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
arm = next(o for o in bpy.context.scene.objects if o.type == "ARMATURE")
# (start from the clip the game plays, when given: its first frame)
if action and action in bpy.data.actions:
    arm.animation_data_create()
    arm.animation_data.action = bpy.data.actions[action]
    bpy.context.scene.frame_set(int(bpy.data.actions[action].frame_range[0]))
arm.show_in_front = True
arm.data.display_type = "STICK"
bpy.context.view_layer.objects.active = arm
for o in bpy.context.scene.objects:
    o.select_set(o == arm)
bpy.ops.object.mode_set(mode="POSE")
bpy.ops.wm.save_as_mainfile(filepath=out)


def setup():
    for area in bpy.context.screen.areas:
        if area.type == "VIEW_3D":
            area.spaces.active.shading.type = "MATERIAL"
            with bpy.context.temp_override(area=area, region=area.regions[-1]):
                bpy.ops.view3d.view_selected()
    return None


bpy.app.timers.register(setup, first_interval=1.0)
