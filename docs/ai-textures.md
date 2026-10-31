# Remade textures

How a remade picture of a game texture gets into the game. This is the plumbing only: the pictures
are yours, made from your own copy of the game, and they stay out of git (`texture_remade/` is in
`.gitignore`). Folders made by hand with Tripo go in the releases all the same, from the disk:
`HANDMADE` in `scripts/enhanced/make_release.py` lists them.

## Where the files go

```
custom_assets/<game>/texture_remade/<tpage>/<name>.png     just that page's texture
custom_assets/<game>/texture_remade/_all/<name>.png        every texture of that name, in any page
```

`<game>` is `jak1`, `jak2` or `jak3`. `<tpage>` and `<name>` are the folder and file name in the
decompiler's dump (`decompiler_out/<game>*/textures/<tpage>/<name>.png`, made with
`"save_texture_pngs": true`), for example `level-default-pris/jakc-armor.png`.

Restart the game to pick up new files. Nothing has to be extracted again: the picture is loaded when
the level is, the common level included (Jak and Daxter live there).

Which one wins, for one texture: a user's edit from the in-game editor's Textures tool
(`texture_edits/`, not for the common level), then `texture_remade/`, then what the level holds, which
may itself be the 4x upscale of `hd_textures.py` (`texture_replacements/`).

Why not `texture_replacements/`: that folder is baked into the levels by a new extraction, and the
decompiler refuses a picture whose colors, brought down to the original's size, are more than 10 of
255 from the original (`TextureDB::replace_texture`). It's meant for the same picture sharper, so a
repainted one is skipped.

## What a picture must keep

- **The name.** The material and everything else follow it. Several different textures can share a
  name across pages: then use the `<tpage>/` folder, not `_all/`.
- **The UVs.** The same part in the same place, with nothing moved, flipped or re-laid out: the models
  are not changed, and their UVs land where the original's parts were.
- **The shape.** Same width : height. Any size of that shape loads; 2x, 4x or 8x are the safe ones. The
  loader refuses another shape and says so in the log.
- **The alpha.** It isn't only cutouts: on many textures that look opaque it says how much of the
  environment reflection shows, and hair and fur use it for their edges. Keep it where it was. The
  dump's alpha is the PS2's, 128 = opaque; a picture with plain 0..255 alpha is halved on load, if the
  game's texture is in the PS2's range.
- **The seams.** Two UV islands that meet on the model must have the same color along the cut, even
  between textures (`jakb-facelft` and `jakb-facert`). A painter or a generator that doesn't see the
  model breaks these first.

Check a folder before installing it:

```
python tools/ai_textures/check_textures.py <folder> --models <decompiler_out>/jak3/levels/common
```

It needs numpy and Pillow. It compares names, shape, alpha, tiling and flips against the dump, and,
given the decompiler's `.glb` models, the color across the UV cuts. It measures seams at the cuts'
vertices only, so a clean report doesn't prove there's no seam between them: look at it in the game too.

## Special cases

- **Jak's own face, hair, arms and fingers in Jak 3** (`jakc-face`, `jakc-hair`, `jakc-arm`,
  `jakc-finger`, `jakc-eyebrow`, and the cutscene `jakchires-*`) are animated textures, blended by the
  game between `-norm` and `-dark` for Dark Jak. Name the picture for the result (`jakc-face.png`, not
  `jakc-face-norm.png`). It's shown while the blend is at rest; as Dark Jak the game's own low
  resolution blend comes back, with a visible change at the start and end of the transformation.
  Remaking the dark version too would take blending the two pictures in `TextureAnimator`.
- **Eyes** are painted each frame by `EyeRenderer` from the iris, pupil and lid textures
  (`jakb-eye`, `autoeye-iris`...) into a 128 x 128 picture. Remaking those works, but only that
  resolution reaches the screen. The eye pass's catchlight and depth sit on top in `merc2.frag`.
- **Other animated textures** (water, the skull gem...) are drawn by the animator: a picture for those
  names has no effect unless they're of the blended kind above.

## How the materials and relief read them

Both follow on their own, with nothing to set up.

- The material of a part comes from the texture's name (`enhanced_char_material` in
  `loader/EnhancedMaterials.h`): skin, hair, metal, leather, cloth, fur, eye, lens. A name it doesn't
  recognize stays as the game has it, with no relief.
- The relief comes from the picture's brightness: bright is raised, dark is sunk, up to 1.2 cm, times
  a factor per material (none on eyes and lenses). So painted-in light and shadow turn into relief too.
  A picture with its lighting baked in will look doubly lit, one painted flat will look calmer.
- Hair's highlight runs across the strands and takes them to run along the texture's v, as the game's
  hair textures do (down the picture). Keep them that way.
- The strength of it all is the "Character detail" slider (`char_detail`, 0 = off).

## Optional relief maps

Beside the picture, with the same page or `_all/` folder, and not needing the picture itself:

```
<name>_n.png    a normal map (OpenGL style: green up the picture, blue out of the surface)
<name>_h.png    a height map (red channel: black low, white 1.2 cm up)
```

Either one replaces the relief from brightness for that texture, also on a texture whose name no
material recognized (it gets the relief, and no other change). If both exist the normal map wins. They
may be any size, and needn't have the picture's. Alpha is ignored. They're loaded the first time a
level with that texture draws, so expect a small hitch then.

They apply to the characters and everything else drawn by merc (`merc2.frag`), not to the levels'
background.
