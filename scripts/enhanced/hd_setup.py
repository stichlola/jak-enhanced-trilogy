"""Jak Enhanced Trilogy: the HD textures, made on this computer from the player's own discs.

    py -3 scripts/enhanced/hd_setup.py [--games jak1 jak2 jak3] [--bin <folder of decompiler.exe>]

The launcher (Jak Enhanced Trilogy.bat) asks once per game and runs this; setup.ps1 -HdTextures too.
1. installs Pillow, NumPy and SciPy (pip) and downloads Real-ESRGAN into out/tools
2. dumps each extracted game's textures (the decompiler with save_texture_pngs)
3. redraws them 4x on the graphics card: the levels' textures (hd_textures.py, into
   custom_assets/<game>/texture_replacements), the HUD and menus (--hud) and the minimap's district
   maps (minimap_maps.py, both into out/enhanced/hud)
4. extracts the levels again, so they carry the HD textures
Re-runs only redraw what's new (out/enhanced-upscale-cache). In the release ZIP the project is data/
and the programs sit next to the launcher.
"""
import argparse
import json
import re
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent
ESRGAN_URL = ("https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/"
              "realesrgan-ncnn-vulkan-20220424-windows.zip")


UPSCALED = re.compile(r"(?:upscaled|pbr) (\d+)/(\d+)")
_shown = -1


def progress(pct):
    """'[ 45%]' at the start of a line: the installer's bar reads it (the compiler prints the same)."""
    global _shown
    pct = max(0, min(100, int(pct)))
    if pct != _shown:
        _shown = pct
        print(f"[{pct:3d}%]", flush=True)


def run(*command, span=None):
    """Runs a command, its output passed through. span=(lo, hi): the step's bar goes from lo to hi,
    moved by hd_textures.py's 'upscaled X/Y' and pbr_maps.py's 'pbr X/Y' lines (other commands: lo at the start, hi at the end)."""
    print(">", " ".join(str(c) for c in command[:3]), "...", flush=True)
    if span:
        progress(span[0])
    with subprocess.Popen([str(c) for c in command], cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, errors="replace", bufsize=1) as proc:
        for line in proc.stdout:
            print(line, end="", flush=True)
            m = UPSCALED.search(line)
            if span and m and int(m.group(2)):
                progress(span[0] + (span[1] - span[0]) * int(m.group(1)) / int(m.group(2)))
    if proc.returncode:
        raise subprocess.CalledProcessError(proc.returncode, command)
    if span:
        progress(span[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--games", nargs="+", default=["jak1", "jak2", "jak3"])
    parser.add_argument("--bin", type=Path, default=ROOT / "out/build/Release/bin")
    args = parser.parse_args()

    run(sys.executable, "-m", "pip", "install", "--quiet", "--upgrade", "pillow", "numpy", "scipy")
    esrgan = ROOT / "out/tools/realesrgan"
    if not (esrgan / "realesrgan-ncnn-vulkan.exe").exists():
        print("Downloading Real-ESRGAN...", flush=True)
        esrgan.mkdir(parents=True, exist_ok=True)
        archive = esrgan / "realesrgan.zip"
        urllib.request.urlretrieve(ESRGAN_URL, archive)
        with zipfile.ZipFile(archive) as z:
            z.extractall(esrgan)
        archive.unlink()

    decompiler = args.bin / "decompiler.exe"
    games = len(args.games)
    for n, game in enumerate(args.games):
        def at(f, n=n):  # a fraction of this game's share of the bar
            return 98 * (n + f) / games
        folders = sorted(p for p in (ROOT / "iso_data").glob(f"{game}*")
                         if re.fullmatch(rf"{game}(_[a-z0-9]+)?", p.name))
        if not folders:
            continue
        # the disc's version, from the folder the extractor made (jak3_pal -> pal, jak3 -> ntsc_v1)
        suffix = folders[0].name[len(game) + 1:]
        version = suffix or "ntsc_v1"
        config = Path("decompiler/config") / game / f"{game}_config.jsonc"
        print(f"[{game}] dumping the textures...", flush=True)
        run(decompiler, config, "iso_data", "decompiler_out", "--version", version, "--config-override",
            json.dumps({"decompile_code": False, "levels_extract": False, "save_texture_pngs": True}),
            span=(at(0), at(0.1)))
        print(f"[{game}] redrawing the textures 4x (the long part)...", flush=True)
        run(sys.executable, HERE / "hd_textures.py", "--game", game, span=(at(0.1), at(0.76)))
        # (the normal and roughness maps from the 4x pictures, on the CPU: a few minutes)
        print(f"[{game}] making the normal and roughness maps...", flush=True)
        run(sys.executable, HERE / "pbr_maps.py", "--game", game, span=(at(0.76), at(0.8)))
        run(sys.executable, HERE / "hd_textures.py", "--game", game, "--hud", span=(at(0.8), at(0.85)))
        print(f"[{game}] extracting the levels again with the HD textures...", flush=True)
        # (Jak 3: the levels' models ripped too, *-lod0.glb: the editor makes the game's own models from them)
        run(decompiler, config, "iso_data", "decompiler_out", "--version", version, "--config-override",
            json.dumps({"decompile_code": False, "levels_extract": True, "rip_levels": game == "jak3"}),
            span=(at(0.85), at(1)))
    run(sys.executable, HERE / "minimap_maps.py", span=(98, 100))
    print("HD textures done.")


if __name__ == "__main__":
    sys.exit(main())
