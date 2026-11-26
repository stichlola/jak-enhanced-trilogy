# Upscale models

`realesrgan-gameai.param` / `.bin`: **4x-GameAI** by Tal (OpenModelDB, https://openmodeldb.info/models/4x-GameAI-2-0,
license WTFPL), an ESRGAN 64nf23nb model trained for PS2-era game textures. Converted to ncnn for
realesrgan-ncnn-vulkan: same graph as `realesrgan-x4plus.param` (Real-ESRGAN, BSD-3), the weights written in its
order (fp16, ncnn tag 0x01306B47). The `.bin` is kept out of git (`*.bin`) and goes into every release
(make_release.py HANDMADE).

Used by `scripts/enhanced/hd_textures.py` and `pbr_maps.py` for Jak 3 (`DEFAULT_MODEL`); Jak 1, Jak 2 and the HUD
keep `realesrgan-x4plus`.
