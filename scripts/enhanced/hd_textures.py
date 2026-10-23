"""Jak 3 Enhanced: 4x AI-upscaled textures as OpenGOAL texture replacements.

    python scripts/enhanced/hd_textures.py [--tpages REGEX] [--gpu 0]

Reads the decompiler's texture dump (decompiler_out/jak3_pal/textures/<tpage>/<name>.png, made
with "save_texture_pngs": true), upscales each one 4x with Real-ESRGAN (wrap-padded so tiling
textures stay seamless) and writes custom_assets/jak3/texture_replacements/<tpage>/<name>.png.
The decompiler picks those up when it extracts the levels again (levels_extract), so the .fr3
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
DUMP = ROOT / "decompiler_out/jak3_pal/textures"
OUT = ROOT / "custom_assets/jak3/texture_replacements"
CACHE = ROOT / "out/enhanced-upscale-cache"
PAD = 16

# Spargus, the desert and its sub-areas, the vehicles, plus Jak/Daxter and the guns.
DEFAULT_TPAGES = r"^(was|des|wca|wcb|wasstad|desert|wcar|wvehicle|jak|daxter|gun|common|lj|dk)"
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tpages", default=DEFAULT_TPAGES, help="regex of texture pages to upscale")
    parser.add_argument("--model", default="realesrgan-x4plus")
    parser.add_argument("--gpu", type=int)
    parser.add_argument("--min-size", type=int, default=16, help="skip textures smaller than this (px)")
    parser.add_argument("--max-size", type=int, default=256, help="skip textures larger than this (px)")
    parser.add_argument("--batch", type=int, default=200)
    parser.add_argument("--tools", type=Path, default=ROOT.parent / "jak3-ue5/tools")
    args = parser.parse_args()

    pick, skip = re.compile(args.tpages), re.compile(EXCLUDE)
    todo = []
    for tpage in sorted(p for p in DUMP.iterdir() if p.is_dir()):
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

    written = 0
    for tpage, png, key in todo:
        scaled = CACHE / args.model / f"{key}.png"
        if scaled.exists():
            (OUT / tpage).mkdir(parents=True, exist_ok=True)
            shutil.copyfile(scaled, OUT / tpage / png.name)
            written += 1
    print(f"wrote {written} replacements to {OUT}")


if __name__ == "__main__":
    sys.exit(main())
