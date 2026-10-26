"""Jak Enhanced Trilogy: 4x AI-upscaled textures as OpenGOAL texture replacements.

    python scripts/enhanced/hd_textures.py [--game jak1|jak2|jak3] [--tpages REGEX] [--gpu 0]

Reads the decompiler's texture dump (decompiler_out/<folder>/textures/<tpage>/<name>.png, made
with "save_texture_pngs": true), upscales each one 4x with Real-ESRGAN (wrap-padded so tiling
textures stay seamless) and writes custom_assets/<game>/texture_replacements/<tpage>/<name>.png.
Decals, cutouts and glass (any texel with alpha < 120/128, see translucent.ps1) get the upscaled
colors but their original alpha, resized smoothly (alpha_restore.ps1): the upscaler would move
and soften their transparency. --opaque-only leaves them at their original resolution. The
decompiler also skips a replacement that doesn't match its texture when
scaled back down (TextureDB::replace_texture), since texture names aren't unique. The decompiler
picks those up when it extracts the levels again (levels_extract), so the .fr3
files carry the HD textures. Results are cached by content hash, so re-runs only do new ones.

Real-ESRGAN and the wrap helper come from the jak3-ue5 checkout (--tools).
"""

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "out/enhanced-upscale-cache"
PAD = 16

# per game: the decompiler's output folder and the pages upscaled by default. Jak 3: Spargus, the
# desert and its sub-areas, the vehicles, plus Jak/Daxter and the guns (Haven still to do);
# Jak 1 and 2: everything
GAMES = {
    "jak1": ("jak1_pal", r"."),
    "jak2": ("jak2_pal", r"."),
    "jak3": ("jak3_pal", r"^(was|des|wca|wcb|wasstad|desert|wcar|wvehicle|jak|daxter|gun|common|lj|dk)"),
}
# HUD, fonts and minimaps are drawn 1:1 on screen or decoded by the game: leave them alone.
EXCLUDE = r"(minimap|hud|font|progress|-sprite$|warp|ocean)"


def png_size(data):
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def run_batch(files, tools, model, gpu):
    esrgan = tools / "realesrgan/bin/realesrgan-ncnn-vulkan.exe"
    wrap = tools / "glb/wrap.ps1"
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for name in ("src", "padded", "scaled"):
            (tmp / name).mkdir()
        for key, path in files.items():
            shutil.copyfile(path, tmp / "src" / f"{key}.png")

        def wrap_step(mode, src, dst):
            subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(wrap), mode,
                            str(src), str(dst), "-Pad", str(PAD), "-Scale", "4"], check=True)

        wrap_step("pad", tmp / "src", tmp / "padded")
        command = [str(esrgan), "-i", str(tmp / "padded"), "-o", str(tmp / "scaled"), "-n", model, "-s", "4",
                   "-m", str(esrgan.parent / "models"), "-f", "png"]
        if gpu is not None:
            command += ["-g", str(gpu)]
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wrap_step("crop", tmp / "scaled", CACHE / model)


def translucent(todo):
    """Keys of the textures with any translucent texel (see translucent.ps1)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for _, png, key in todo:
            shutil.copyfile(png, tmp / f"{key}.png")
        result = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                 str(Path(__file__).with_name("translucent.ps1")), str(tmp)],
                                check=True, capture_output=True, text=True)
    return {Path(name).stem for name in result.stdout.split()}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", choices=sorted(GAMES), default="jak3")
    parser.add_argument("--tpages", help="regex of texture pages to upscale (default: per game)")
    parser.add_argument("--model", default="realesrgan-x4plus")
    parser.add_argument("--gpu", type=int)
    parser.add_argument("--min-size", type=int, default=16, help="skip textures smaller than this (px)")
    parser.add_argument("--max-size", type=int, default=256, help="skip textures larger than this (px)")
    parser.add_argument("--batch", type=int, default=200)
    parser.add_argument("--tools", type=Path, default=ROOT.parent / "jak3-ue5/tools")
    parser.add_argument("--opaque-only", action="store_true", help="skip translucent textures")
    args = parser.parse_args()
    global OUT
    folder, default_tpages = GAMES[args.game]
    dump = ROOT / "decompiler_out" / folder / "textures"
    OUT = ROOT / "custom_assets" / args.game / "texture_replacements"

    pick, skip = re.compile(args.tpages or default_tpages), re.compile(EXCLUDE)
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
        run_batch(batch, args.tools, args.model, args.gpu)
        print(f"  upscaled {min(start + args.batch, len(keys))}/{len(keys)}", flush=True)

    # translucent ones: the upscaled colors with the original alpha, cached apart
    alpha_dir = CACHE / f"{args.model}-alpha"
    alpha_dir.mkdir(parents=True, exist_ok=True)
    pending = {key: unique[key][1] for key in see_through
               if not (alpha_dir / f"{key}.png").exists() and (CACHE / args.model / f"{key}.png").exists()}
    if pending:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "orig").mkdir()
            (tmp / "scaled").mkdir()
            for key, png in pending.items():
                shutil.copyfile(png, tmp / "orig" / f"{key}.png")
                shutil.copyfile(CACHE / args.model / f"{key}.png", tmp / "scaled" / f"{key}.png")
            subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            str(Path(__file__).with_name("alpha_restore.ps1")), str(tmp / "orig"),
                            str(tmp / "scaled")], check=True)
            for key in pending:
                shutil.copyfile(tmp / "scaled" / f"{key}.png", alpha_dir / f"{key}.png")
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
