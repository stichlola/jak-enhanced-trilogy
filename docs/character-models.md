# Replacing a character's model (Jak 3)

Engine support only: no replacement model exists yet and this path has **never been run with one**.
The code was checked by reading and by compiling, not in game.

**The honest summary:** a body swap is realistic, and the path handles it. **Faces and animation
fidelity are the risk.** The game's facial animation only works on the game's own face mesh, and its
body animations were authored for the game's proportions. A new character will likely look great
standing still and worse in motion and in cutscenes, by an amount nobody can predict before trying.

## The paths that exist

| Path | What it does | Use for |
|---|---|---|
| `custom_assets/<game>/merc_replacements/<model>.glb` | Read at extraction; replaces one merc model by name in every level holding it. In the releases (`HANDMADE` in `make_release.py`), and after an update `refresh_models.ps1` extracts again the levels holding a changed one | **This document** |
| `custom_assets/<game>/models/<level>/*.glb`, `goalc/build_actor` | Adds a new model under a new name, needs GOAL to use it | New actors, not replacements |
| `EnhancedImport.cpp`, `custom_assets/<game>/scenery/` | Static scenery glbs, no skin | Not for characters |
| `game/assets/enhanced/game_models.json` | The in-game editor's list of placeable models | Not related |

## What the upstream path already did, and what was added

Already there: replaces by file name, turns triangles and base-colour textures into a `no_strip`
merc model, takes weights from the nearest old vertex or from the glb (`enable_custom_weights` in the
node's extras), copies old eye and blend-shape draws on request, reads envmap materials
(`KHR_materials_specular` with a metallic-roughness texture).

Missing pieces, now added, each only active for a model that asks for it in its json, except where
noted:

- **A switch per model**: `<model>.json` next to the glb (below). Without one, nothing changes.
- **Joints by name** (`joints_by_name`). The glb's `JOINTS_0` used to be taken as the game's joint
  numbers, which only holds if the skin keeps the game's exact order. Now each skin joint is looked up
  by name in the art group's skeleton. A helper bone the game lacks goes with its nearest ancestor,
  with a warning. A joint more than 5 cm off the game's rest pose also gets a warning.
- **All the bones** (Jak 3 only, every `no_strip` model, `Merc2.cpp`). The game only sends the matrices
  its own mesh uses, so a vertex on any other joint read garbage. They are now picked up from where
  the game keeps them.
- **The game's effect switches** (Jak 3 only, same condition). The game enables effects by number, and
  a replacement's effects aren't the game's: the copied face could be switched off by whatever was
  old effect 2 (the beard). Now all follow effect 0.
- **A face when no blend shape is playing.** `copy_mod_draws` kept the face only in the mod draws, which
  are used only while the game sends weights, so otherwise it was missing. It now also gets plain
  draws of the neutral vertices. Eyes (`copy_eye_draws`) also stay during blend shapes.
- **Lods** (`all_lods`): `-lod1`, `-lod2` without a glb of their own become copies of the replaced lod0,
  without extra memory.
- **Fixes**: several meshes sharing a material lost all but the last one's triangles; a primitive with
  no skin data misaligned the weights of those after it; `max_draws` didn't count mod draws.

## Steps

1. **Get the rig.** Rip the original: `task rip-levels` (the extraction with `rip_levels` on), then `decompiler_out/jak3/levels/<level>/<model>.glb`. Which model: Jak in normal play is
   `jakb-c-lod0` (`common`, `target-geo jakc`, `skel-jchar-c`), not `jakb-lod0`, which has only the two
   eye triangles. Cutscenes use `jak-highres-lod0`, `jakc-highres-lod0`, `darkjak-highres-lod0`: separate
   files. For anything else, Merc2's debug window lists the names of what's being drawn ("Debug").

2. **Model on it.** In Blender, import the rip without shape keys and keep its armature **unmoved, in
   its rest pose**. The game skins from its own rest pose and ignores the glb's bind matrices, so the
   mesh must sit around the skeleton exactly as ripped: same scale (1 unit = 1 game metre = 4096 game
   units, Jak is about 3.1 tall), +y up. Apply all object transforms.

3. **Weight it.** Use the game's joint names; don't rename. Up to 4 weights per vertex are read, the
   smallest is dropped and the rest renormalized, as merc does three. Unweighted vertices go to `main`.

4. **Export** glb with: triangles, one skin, one armature, 8-bit joints (Blender's default below 256
   bones), UVs, normals, images embedded, base colour textures with a sampler. No shape keys or
   animation needed. Vertex colours multiply the lighting: without `COLOR_0` the path uses the game's
   neutral grey (0x80), so normally leave them out. tinygltf warnings stop the extraction, on purpose
   upstream: fix the file.

5. **Save** as `custom_assets/jak3/merc_replacements/<model>.glb` and add `<model>.json`, for example:

   ```json
   { "joints_by_name": true, "copy_eye_draws": true, "copy_mod_draws": true }
   ```

   | Key | Default | Effect |
   |---|---|---|
   | `enabled` | true | false: keep the game's model, leaving the glb in place |
   | `joints_by_name` | false | The glb's weights, joints matched by name. Use this one |
   | `custom_weights` | false | The glb's weights, joint order already the game's |
   | `copy_eye_draws` | false | Keep the game's eyes |
   | `copy_mod_draws` | false | Keep the game's blend-shape face |
   | `all_lods` | false | This `-lod0` also stands in for lower lods without a glb |

   Neither weight option: each vertex copies the nearest old vertex's weights. That's fine for very
   similar shapes and is the quickest first test. Nothing in this folder is tracked by git, and it must
   stay so if it derives from the game's data.

6. **Re-extract.** The swap happens at extraction, so `out/jak3/fr3/*.fr3` must be rebuilt, which is
   slow since all levels go through it. Back the folder up first:

   ```
   decompiler ./decompiler/config/jak3/jak3_config.jsonc ./iso_data ./decompiler_out --version ntsc_v1 --config-override "{\"decompile_code\": false, \"levels_extract\": true, \"allowed_objects\": []}"
   ```

   (`task extract` does it.) The log says
   `Replacing <model> for <level>` and any joint warnings. To undo: `"enabled": false`, or remove the
   glb, and extract again, or restore the backup.

7. **Check in game** (below). Do one model at a time, and start with something without a face to
   lose: an enemy or a vehicle (see [vehicle-models.md](vehicle-models.md)).

## Limits, and what breaks

**Face (high risk).** Blend shapes can't be carried over: the path doesn't read morph targets, and they
would have to match the game's targets one for one in meaning. The choices are:

- keep the game's face (`copy_mod_draws`): it still moves, but it's the old low-poly mesh, so the new
  head has to be built around it exactly, and it won't match in detail or style. Jak's `jakb-c-lod0`
  does have blend targets;
- or model the face into the new mesh: it is detailed but frozen: no lip sync, no expressions. Fine for
  a masked enemy, very visible on Jak in a cutscene.

The character smoothing skips `no_strip` models, so it doesn't apply to either the new mesh or, in a
replaced model, the copied face.

**Eyes (high risk).** Eye animation works by the game painting eye textures on the draws marked with an
eye id. A new mesh can't mark any, so `copy_eye_draws` keeps the game's eyeball geometry where it was:
the new head needs its sockets exactly there. Without it the eyes don't move.

**Animation fidelity (high risk).** Only the joints are shared. The animations were made for the game's
proportions, so a slimmer or bigger body will clip, hands won't meet what they hold, feet may slide.
Merc takes three weights per vertex against the usual four, which shows at shoulders and hips. The
joint positions can't be changed to compensate: animation data sets them.

**Parts the game hides and shows.** `setup-masks` turns old effects on and off (Jak's beard is effect
2). A replacement is one effect, so those no longer work: the beard setting does nothing, a hidden
accessory stays. If the game hides old effect 0, the whole model goes. The game hiding and showing
whole models, or changing Jak's look by switching to another model, still works but needs one
replacement per model.

**Cloth.** Jak's skirt, sash and scarf (`:clothing` in `skel-jchar-c`) are separate meshes simulated
elsewhere, and they stay, over the new body. Removing them needs GOAL changes.

**Shadows.** The sun's shadow is the old shape from its own shadow mesh (`jakb-c-shadow-mg`); Enhanced's
screen-space shadows, where on, follow the new one.

**Materials.** Only base colour and the envmap extension. Replaced textures have other names, so
per-texture Enhanced materials won't recognize them. No normal maps in this path.

**Ripple and texture scrolling.** Don't use `copy_mod_draws` on a model with them: that path reads the
game's data per old effect and new effect numbers don't line up. Blend shapes are fine.

**Levels.** A replacement applies wherever that model name exists (each file is replaced once per
level), and the fr3s get bigger. A character model budget of about 10000 faces (`sheet.py`'s
`BUDGETS`) against the game's 3000-5000 is safe; much more is untested.

**Jak 1 and 2.** The two Merc2 changes only apply to Jak 3. The extraction changes apply to all, but do
nothing without a replacement; replacements already in use get the three fixes above, and no others
without a json.

## How to test

No new model is needed to test the mechanics, using the game's own (local files only):

1. In Blender, import the ripped `jakb-c-lod0.glb` and export it again as above, unchanged, to
   `custom_assets/jak3/merc_replacements/jakb-c-lod0.glb`, with the json from step 5. Re-extract.
2. The log should show `Replacing jakb-c-lod0 for ...`, with no joint warnings. With warnings, the
   export moved the rig.
3. In game, Jak should look about the same, with z-fighting on the face (since both old and new faces
   are drawn) and animating correctly. Watch elbows, knees, fingers, the gun, and Daxter on his shoulder.
   Spinning or stretched parts mean a wrong joint: with `{"custom_weights": true}` instead you can see
   what wrong order does.
4. Remove `copy_mod_draws` and the double face goes, leaving a frozen one; there should be no missing
   parts. Stand idle and then trigger a blend-shape scene with `copy_mod_draws` back on: the face must
   be there both before and during.
5. `"enabled": false` and re-extract brings back the game's model.
