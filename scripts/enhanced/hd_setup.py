"""Jak Enhanced Trilogy: the HD textures, made on this computer from the player's own discs.

    py -3 scripts/enhanced/hd_setup.py [--games jak1 jak2 jak3] [--bin <folder of decompiler.exe>] [--refresh]

The launcher (Jak Enhanced Trilogy.bat) asks once per game and runs this; setup.ps1 -HdTextures too.
1. installs Pillow, NumPy and SciPy (pip) and downloads Real-ESRGAN into out/tools
2. dumps each extracted game's textures (the decompiler with save_texture_pngs)
3. redraws them 4x on the graphics card: the levels' textures (hd_textures.py, into
   custom_assets/<game>/texture_replacements), the HUD and menus (--hud) and the minimap's district
   maps (minimap_maps.py, both into out/enhanced/hud)
4. extracts the levels again, so they carry the HD textures
Re-runs only redraw what's new (out/enhanced-upscale-cache). In the release ZIP the project is data/
and the programs sit next to the launcher.

--refresh (the launcher, once after each update): only the games whose HD textures were made by an
older version of these scripts (out/<game>/hd-version.txt against HD_VERSION): the textures the
change touches are redrawn and the levels extracted again. Nothing, quietly, for a game without HD
textures or with them up to date.
"""
import argparse
import importlib.util
import json
import os
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


# How hd_textures.py makes them: bumped when a change must reach the HD textures already made on the
# players' computers (--refresh).
# 2: the translucent textures' colors upscaled from an opaque copy (the upscaler blacked out what
#    lies under alpha 0: Daxter's cutscene face, every eyelid, hair and teeth drawn opaque)
HD_VERSION = "2"

UPSCALED = re.compile(r"upscaled (\d+)/(\d+)")
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
    moved by hd_textures.py's 'upscaled X/Y' lines (other commands: lo at the start, hi at the end)."""
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


def stamp(game):
    return ROOT / "out" / game / "hd-version.txt"


def outdated(game):
    """HD textures made for this game by an older version of these scripts"""
    made = ROOT / "custom_assets" / game / "texture_replacements"
    if not made.is_dir() or next(made.glob("*/*.png"), None) is None:
        return False
    return not stamp(game).exists() or stamp(game).read_text().strip() != HD_VERSION


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--games", nargs="+", default=["jak1", "jak2", "jak3"])
    parser.add_argument("--bin", type=Path, default=ROOT / "out/build/Release/bin")
    parser.add_argument("--refresh", action="store_true",
                        help="only games whose HD textures an older version made: what changed, made again")
    args = parser.parse_args()

    hidden = bool(os.environ.get("JAK_ENHANCED_HIDDEN"))  # the launcher's window shows "@@step" lines
    if args.refresh:
        args.games = [game for game in args.games if outdated(game)]
        if not args.games:
            return 0
        names = ", ".join(game.replace("jak", "Jak ") for game in args.games)
        text = (f"{names}: this version makes some HD textures better (faces, eyelids, hair). They are "
                "redrawn and the levels made again: 15-40 minutes, only this once. You can leave it running.")
        print(f"@@step {text}" if hidden else text, flush=True)

    # (a refresh: Python's packages are there since the first time)
    if not args.refresh or any(importlib.util.find_spec(m) is None for m in ("PIL", "numpy", "scipy")):
        run(sys.executable, "-m", "pip", "install", "--quiet", "--upgrade", "pillow", "numpy", "scipy")
    esrgan = ROOT / "out/tools/realesrgan"
    # (or the jak3-ue5 checkout's, as hd_textures.py finds it: nothing to download then)
    elsewhere = ROOT.parent / "jak3-ue5/tools/realesrgan/bin/realesrgan-ncnn-vulkan.exe"
    if not (esrgan / "realesrgan-ncnn-vulkan.exe").exists() and not elsewhere.exists():
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
            if args.refresh:
                print(f"[{game}] the disc's data (iso_data) isn't here: its HD textures stay as they are.")
            continue
        # the disc's version, from the folder the extractor made (jak3_pal -> pal, jak3 -> ntsc_v1)
        suffix = folders[0].name[len(game) + 1:]
        version = suffix or "ntsc_v1"
        config = Path("decompiler/config") / game / f"{game}_config.jsonc"
        # (a refresh: the dump of the first time is still there, unless it was deleted)
        dumped = any(next(folder.glob("textures/*/*.png"), None) is not None
                     for folder in (ROOT / "decompiler_out").glob(f"{game}*"))
        if not (args.refresh and dumped):
            print(f"[{game}] dumping the textures...", flush=True)
            run(decompiler, config, "iso_data", "decompiler_out", "--version", version, "--config-override",
                json.dumps({"decompile_code": False, "levels_extract": False, "save_texture_pngs": True}),
                span=(at(0), at(0.1)))
        print(f"[{game}] redrawing the textures 4x (the long part)...", flush=True)
        run(sys.executable, HERE / "hd_textures.py", "--game", game, span=(at(0.1), at(0.8)))
        run(sys.executable, HERE / "hd_textures.py", "--game", game, "--hud", span=(at(0.8), at(0.85)))
        print(f"[{game}] extracting the levels again with the HD textures...", flush=True)
        # (Jak 3: the levels' models ripped too, *-lod0.glb: the editor makes the game's own models from them)
        run(decompiler, config, "iso_data", "decompiler_out", "--version", version, "--config-override",
            json.dumps({"decompile_code": False, "levels_extract": True, "rip_levels": game == "jak3"}),
            span=(at(0.85), at(1)))
        stamp(game).write_text(HD_VERSION)
    if not args.refresh:
        run(sys.executable, HERE / "minimap_maps.py", span=(98, 100))
    print("HD textures done.")


if __name__ == "__main__":
    sys.exit(main())
