"""Jak Enhanced Trilogy: 4x AI-upscaled textures as OpenGOAL texture replacements.

    python scripts/enhanced/hd_textures.py [--game jak1|jak2|jak3] [--tpages REGEX] [--gpu 0] [--hud]

Reads the decompiler's texture dump (decompiler_out/<folder>/textures/<tpage>/<name>.png, made
with "save_texture_pngs": true), upscales each one 4x with Real-ESRGAN (wrap-padded so tiling
textures stay seamless) and writes custom_assets/<game>/texture_replacements/<tpage>/<name>.png.
Decals, cutouts and glass (any texel with alpha < 120/128, see translucent()) get the upscaled
colors but their original alpha, resized smoothly (restore_alpha): the upscaler would move
and soften their transparency. --opaque-only leaves them at their original resolution. The
decompiler also skips a replacement that doesn't match its texture when
scaled back down (TextureDB::replace_texture), since texture names aren't unique. The decompiler
picks those up when it extracts the levels again (levels_extract), so the .fr3
files carry the HD textures. Results are cached by content hash, so re-runs only do new ones.

--hud does the HUD and menu art instead (the "...-minimap" pages, Jak 1's Hud and zoomerhud: health,
weapons, the pause menu's pictures...; not the fonts, nor the minimap's maps and icons, which have
their own scripts) and writes it to out/enhanced/hud/<game>/<tpage>/<name>.png: the game loads it
as the level loads (EnhancedMapTextures.cpp) and keeps drawing it at its own size, so no new
extraction is needed.

Real-ESRGAN comes from out/tools (hd_setup.py downloads it) or the jak3-ue5 checkout.
"""

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "out/enhanced-upscale-cache"
PAD = 16

GAMES = ("jak1", "jak2", "jak3")
# HUD, fonts and minimaps are drawn 1:1 on screen or decoded by the game: leave them alone.
EXCLUDE = r"(minimap|hud|font|progress|-sprite$|warp|ocean)"
# --hud: the HUD and menu pages, but not the fonts nor the minimap's own art
HUD_TPAGES = r"(-minimap$|^Hud$|^zoomerhud$)"
HUD_EXCLUDE = r"(^map-|^mini-map-icons$|font)"


def find_dump(game):
    """The decompiler's texture dump of a game (decompiler_out/jak3_pal, jak3...: the disc's version)"""
    dumps = [folder / "textures" for folder in (ROOT / "decompiler_out").glob(f"{game}*")
             if re.fullmatch(rf"{game}(_[a-z0-9]+)?", folder.name) and (folder / "textures").is_dir()]
    if dumps:  # the fullest, if there are several
        return max(dumps, key=lambda d: sum(1 for _ in d.glob("*/*.png")))
    raise SystemExit(f"no texture dump of {game} in decompiler_out (decompile with save_texture_pngs)")


def find_esrgan():
    """Real-ESRGAN: downloaded by hd_setup.py into out/tools, or the jak3-ue5 checkout's"""
    for exe in (ROOT / "out/tools/realesrgan/realesrgan-ncnn-vulkan.exe",
                ROOT.parent / "jak3-ue5/tools/realesrgan/bin/realesrgan-ncnn-vulkan.exe"):
        if exe.exists():
            return exe
    raise SystemExit("Real-ESRGAN not found: run scripts/enhanced/hd_setup.py")


def png_size(data):
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def run_batch(files, model, gpu):
    esrgan = find_esrgan()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for name in ("padded", "scaled"):
            (tmp / name).mkdir()
        # each texture surrounded by copies of itself, so tiling ones stay seamless
        for key, path in files.items():
            rgba = np.asarray(Image.open(path).convert("RGBA"))
            Image.fromarray(np.pad(rgba, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="wrap")).save(
                tmp / "padded" / f"{key}.png")
        command = [str(esrgan), "-i", str(tmp / "padded"), "-o", str(tmp / "scaled"), "-n", model, "-s", "4",
                   "-m", str(esrgan.parent / "models"), "-f", "png"]
        if gpu is not None:
            command += ["-g", str(gpu)]
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        border = PAD * 4
        for key in files:
            scaled = Image.open(tmp / "scaled" / f"{key}.png")
            scaled.crop((border, border, scaled.width - border, scaled.height - border)).save(
                CACHE / model / f"{key}.png")


def translucent(todo):
    """Keys of the textures with any translucent texel (PS2 alpha below 120 of 128): decals, cutouts
    and glass, often blended so that even the color of "transparent" texels shows"""
    keys = set()
    for _, png, key in todo:
        if np.asarray(Image.open(png).convert("RGBA"))[..., 3].min() < 120:
            keys.add(key)
    return keys


def restore_alpha(original, scaled, out):
    """The upscaled colors with the original alpha resized smoothly: the upscaler would move and soften
    the transparency, and cutouts and decals must blend as in the game"""
    big = Image.open(scaled).convert("RGBA")
    alpha = Image.open(original).convert("RGBA").getchannel("A").resize(big.size, Image.BICUBIC)
    big.putalpha(alpha)
    big.save(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", choices=GAMES, default="jak3")
    parser.add_argument("--tpages", help="regex of texture pages to upscale (default: per game)")
    parser.add_argument("--model", default="realesrgan-x4plus")
    parser.add_argument("--gpu", type=int)
    parser.add_argument("--min-size", type=int, default=16, help="skip textures smaller than this (px)")
    parser.add_argument("--max-size", type=int, default=256, help="skip textures larger than this (px)")
    parser.add_argument("--batch", type=int, default=200)
    parser.add_argument("--opaque-only", action="store_true", help="skip translucent textures")
    parser.add_argument("--hud", action="store_true", help="the HUD and menu art (out/enhanced/hud)")
    args = parser.parse_args()
    global OUT
    default_tpages = r"."
    dump = find_dump(args.game)
    OUT = ROOT / "custom_assets" / args.game / "texture_replacements"
    if args.hud:
        OUT = ROOT / "out" / "enhanced" / "hud" / args.game
        default_tpages = HUD_TPAGES
        args.max_size = max(args.max_size, 512)

    pick, skip = re.compile(args.tpages or default_tpages), re.compile(HUD_EXCLUDE if args.hud else EXCLUDE)
    todo = []
    for tpage in sorted(p for p in dump.iterdir() if p.is_dir()):
        if not pick.search(tpage.name) or skip.search(tpage.name):
            continue
        for png in sorted(tpage.glob("*.png")):
            if skip.search(png.stem):
                continue
            data = png.read_bytes()
            w, h = png_size(data)
            if min(w, h) < args.min_size or max(w, h) > args.max_size:
                continue
            todo.append((tpage.name, png, hashlib.sha1(data).hexdigest()))

    (CACHE / args.model).mkdir(parents=True, exist_ok=True)
    unique = {key: (tpage, png, key) for tpage, png, key in todo}
    see_through = translucent(list(unique.values()))
    if args.opaque_only:
        for tpage, png, key in todo:
            if key in see_through:  # drop a replacement left by an older run
                (OUT / tpage / png.name).unlink(missing_ok=True)
        todo = [entry for entry in todo if entry[2] not in see_through]
        print(f"{len(see_through)} translucent textures left at their original resolution")
        see_through = set()
    missing = {}
    for _, png, key in todo:
        if not (CACHE / args.model / f"{key}.png").exists():
            missing[key] = png
    print(f"{len(todo)} textures in {len({t for t, _, _ in todo})} pages, {len(missing)} to upscale")

    keys = list(missing)
    for start in range(0, len(keys), args.batch):
        batch = {key: missing[key] for key in keys[start:start + args.batch]}
        run_batch(batch, args.model, args.gpu)
        print(f"  upscaled {min(start + args.batch, len(keys))}/{len(keys)}", flush=True)

    # translucent ones: the upscaled colors with the original alpha, cached apart
    alpha_dir = CACHE / f"{args.model}-alpha"
    alpha_dir.mkdir(parents=True, exist_ok=True)
    pending = {key: unique[key][1] for key in see_through
               if not (alpha_dir / f"{key}.png").exists() and (CACHE / args.model / f"{key}.png").exists()}
    if pending:
        for key, png in pending.items():
            restore_alpha(png, CACHE / args.model / f"{key}.png", alpha_dir / f"{key}.png")
        print(f"{len(pending)} translucent textures given back their original alpha")

    written = 0
    for tpage, png, key in todo:
        scaled = (alpha_dir if key in see_through else CACHE / args.model) / f"{key}.png"
        if scaled.exists():
            (OUT / tpage).mkdir(parents=True, exist_ok=True)
            shutil.copyfile(scaled, OUT / tpage / png.name)
            written += 1
    print(f"wrote {written} replacements to {OUT}")


if __name__ == "__main__":
    sys.exit(main())
