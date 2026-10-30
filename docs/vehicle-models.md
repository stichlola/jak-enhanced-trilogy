# Jak 3 vehicles: what a high-detail replacement needs

Research note, nothing generated. It lists what a vehicle replacement would involve so that the
decision to spend Tripo credits can be made later. Replacing the mesh goes through the path in
[character-models.md](character-models.md); a vehicle is simpler than a character because it has
no face and its parts are close to rigid.

Sources: `goal_src/jak3/levels/desert/wvehicle/wcar*.gc`, `goal_src/jak3/levels/city/traffic/vehicle/car.gc`,
the `.glb` files the decompiler rips (`rip_levels`, `decompiler_out/jak3/levels/<level>/<model>.glb`,
strips counted as triangles), and `jak3-ue5/tools/tripo/sheet.py` plus its `tasks.json` log.
`game_models.json` doesn't help: it is the in-game editor's list of placeable models and it leaves
vehicles out on purpose.

## What is being replaced

A vehicle is several merc models, not one. Each file below is one replacement:

- the body, `<name>-lod0`, skinned to its joints;
- the wheel, `<name>-wheel-lod0`: its own actor, spawned four times by `spawn-wheels!`
  (`wvehicle-util.gc`) and placed from `vehicle-wheel-info`, so it isn't part of the body;
- the wheel at speed, `<name>-wheel-blur-lod0`, where it exists. Without a replacement the old
  blurred wheel shows at speed, which may look acceptable or not;
- sometimes `-wheel-fma-lod0` (cutscene wheel), a separate gun model, and debris. The game's shadow
  (`<name>-shadow-mg`) is separate data and stays the old shape.

## Wasteland cars

Sizes are game metres (x width, y height, z length) of the model's vertices in its rest pose,
triangles are the game's own. Every skeleton also starts with `align prejoint`, and the joint order
matters in the game (below).

| Vehicle | Body model | Tris | Size | Wheel (tris) | Blur | Moving joints after `main` |
|---|---|---|---|---|---|---|
| Toad | `toad-lod0` | 2448 | 7.1 x 3.8 x 9.4 | `toad-wheel` (342) | yes | `LR_axle L_front_shock R_front_shock RR_axle rocket steer_wheel_L LR_shock_top LR_shock_bottom RR_shock_top RR_shock_bottom steer_wheel_R` |
| Snake (also Mirage) | `snake-lod0` | 3972 | 6.7 x 2.8 x 8.1 | `snake-wheel` (288) | yes, + fma | `L_R_axle L_F_axle R_F_axle R_R_axle steer_wheel_L LR_shock_top LR_shock_mid LF_shock_top LF_shock_mid RR_shock_top RR_shock_mid RF_shock_top RF_shock_mid steer_wheel_R L_gun R_gun` |
| Rhino | `rhino-lod0` | 2694 | 10.7 x 6.4 x 12.5 | `rhino-wheel` (276) | no, fma | `front_axle_L rear_axle_L front_axle_R rear_axle_R R_steer_wheel gun gun_tip L_steer_wheel` |
| Scorpion | `scorpion-lod0` | 3770 | 9.0 x 4.8 x 10.7 | `scorpion-wheel` (288) | yes, + fma | `turret gun gunend front_axle_L rear_axle_L arm_top/mid/bottom_front_L arm_top/mid/bottom_rear_L front_axle_R rear_axle_R arm_top/mid/bottom_front_R arm_top/mid/bottom_rear_R steer_wheel` |
| Fox (also X-ride) | `fox-lod0` | 3074 | 4.1 x 3.0 x 8.8 | `fox-wheel` (366) | yes | `L_R_axle L_F_axle R_F_axle R_R_axle steer_wheel LR_shock_top LR_shock_mid LF_shock_top LF_shock_mid RR_shock_top RR_shock_mid RF_shock_top RF_shock_mid L_gun R_gun` |
| Turtle | `turtle-lod0` | 2520 | 4.8 x 5.5 x 5.6 | `turtle-wheel` (178) | yes, + fma | `axle_Lrear axle_Lfront axle_Rfront axle_Rrear steer_wheel flag_a flag_b flag_c flag_d tail_a tail_b L_R_strut_top L_R_strut_mid L_F_strut_top L_F_strut_mid` |
| Marauder's car | `interceptor-lod0` (also `interceptor-b`) | 2608 | 4.6 x 3.7 x 11.6 | `interceptor-wheel` (216) | yes | `L_R_axle L_F_axle gun_y_pivot gun_x_pivot gun_barrel R_F_axle R_R_axle hatch` |

Also: `scorpion-gun-lod0` (1468 tris, `main base_Y_rotate gun_X_rot_Z_trans gun_Z_rotate muzzle`), the
marauder's debris (`interceptor-debris-nut/panel/ring/rod`), and the catapult in
`levels/desert/chase/wcar-catapult.gc` (not inspected). The size figures come from every vertex of
the model, so an outlying part can inflate them (the rhino especially): use them to check scale,
not as the car's real footprint.

### How the parts move

Toad's setup in `wcar-toad.gc` is typical: `joint-mod-rotate-local` on the axles (5, 6, 4, 7),
another on the shock tops (10, 12), `joint-mod-set-local` on the shock bottoms (11, 13), which are
stretched to reach their wheel, and `rider-hand-joint-array` holds the hand joint (14,
`steer_wheel_R`). All of them are **numbers in GOAL**, one more than the joint's place in the list
counting `align` as 0 (so Toad's `L_front_shock` and `R_front_shock` are front axles despite their
names), and the debris code uses numbers too (`joint-exploder-static-joint-params`, `wcar.gc`). So:

- keep every joint with the same name, and leave the rest pose as ripped: the wheel suspension
  and the riders' hands assume the joint positions, and animation data moves them from there;
- anything that must move has to be its own part in the mesh, weighted fully to its joint. A model
  from Tripo is one lump, so the shocks, guns, flags, hatch and steering wheels have to be cut out
  or rebuilt by hand (below). The wheel arches must leave room for the wheel's travel.

### Scale

The ripped glb is in game metres, and replacement glbs are read the same way (1 unit = 4096 game
units). Physics doesn't read the mesh: the collision sphere, wheel radius and position come from
the `*v-<name>-constants*` tables in `wcar.gc`. Toad's `radius` is 4915.2 (1.2 m) and `scale` 1.0 at
the front, 1.5 at the rear, so the same wheel model is drawn at different sizes per axle. **The new
wheel has to fit the old wheel's bounds exactly**, centred on its origin with the same spin axis
(x), or it will sink into the ground or float. Build the body around the ripped body in Blender,
to the same size; nothing needs fitting in the code.

## City vehicles

Same path but no separate wheels, and they have three lods each. See `car.gc` for the joint mods.

| Vehicle | Models | Tris (lod0) | Size | Moving joints after `main` |
|---|---|---|---|---|
| Hellcat (the player's) | `hellcat-lod0/1/2` | 4014 | 8.4 x 3.8 x 10.9 | `frontgun turret turretA` |
| Car A (B, C alike) | `cara-lod0/1/2` | 1189 | 5.0 x 3.5 x 8.4 | `main_ frontfinl_ frontfinr_ rearfinl_ rearrudderl_ rearfinr_ rearrudderr_ rudder_ wheel_left_ wheel_right_` |
| Bike A (B, C alike) | `bikea-lod0/1/2` | 752 | 2.1 x 2.4 x 5.3 | `main_ rudder_ tailfinr_ tailfinl_ frontfinr_ frontfinl_ rearbrakel_ rearbraker_` |
| Transports | `transport-lod0/1/2` (660 tris), `desert-transport-lod0` (2818) | | desert: 11.5 x 12.3 x 22.5 | `hatch` |

The lower lods need their own glb, or `"all_lods": true` in the model's json (see
[character-models.md](character-models.md)); otherwise the old model pops back in at a distance.

## Face budget

`sheet.py`'s `BUDGETS` say 8000 faces for a player vehicle and 1500 for a small prop. That suits this:

- body: **8000**, about 2 to 3 times the game's 2500-4000. The character smoothing doesn't apply to
  vehicles;
- wheel: **1000-1500**, 3 to 8 times the game's 180-370, but it's drawn four times, so 4000-6000
  triangles per car in wheels alone. 1000 is enough;
- a wasteland car in total: about 12000 triangles against the game's 3500-5000. Only a few appear at
  once. There are no lods for them, so it's the same count at any distance.

## Credits

Costs from `sheet.py` (`image`, default guard `--max-cost 10`; `model`, `--max-cost 60`) and its
`tasks.json` log: **5 credits** an image (24 logged), **40 credits** a model (13 logged; 3 at 50).
The price doesn't depend on how many pieces are on a sheet or on the face limit.

| Plan | Credits |
|---|---|
| One sheet with body and wheel together | 45-55 |
| Body and wheel on separate sheets (better detail for each) | 90-110 |
| Each failed image | 5 more |
| Each failed model | 40-50 more |

The balance last noted in the project is about 150 credits, so 1 to 3 vehicles, realistically 2 with
a retry. Check with `sheet.py balance` first. Start with one car, Toad or Fox, whose rig is simple,
before spending on the rest. The Hellcat needs no wheels and the least rigging, so it's the cheapest
overall but the one seen closest.

## The work after generating

The credits are the small part. Tripo returns an unrigged textured mesh, so per vehicle:

1. Blender: load the ripped `<model>.glb` as the reference and fit the new mesh to it (same size,
   front on +z, y up; check against the rip). `jak3-ue5/tools/blender/parts_kit.py` cuts sheets into pieces.
2. Cut the moving parts and bind them to the joints, at weight 1, keeping the ripped armature as is.
   Shocks and struts that stretch are best kept from the original model.
3. Save `custom_assets/jak3/merc_replacements/<model>.glb` and its json, re-extract, and test
   following [character-models.md](character-models.md).

Nothing here has been tried on a vehicle.
