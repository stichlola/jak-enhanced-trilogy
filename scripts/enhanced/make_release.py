"""Jak Enhanced Trilogy: the release ZIP for players (nothing to build or install).

    python scripts/enhanced/make_release.py [--bin out/build/Release-static/bin] [--version v1.0]

Lays out what OpenGOAL's own releases have (.github/scripts/releases/extract_build_windows.sh):
gk.exe, goalc.exe and extractor.exe (the statically linked build: no runtime DLLs), and next to them
data/ with the decompiler's config, the game code, the assets, the shaders and the custom levels; the
programs find data/ by themselves. Plus the one-click launcher and the ISO folder.

Only files tracked by git go in, so nothing from the games (extracted data, HD textures made from
the game's own, settings) can slip into it.
"""

import argparse
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAME = "Jak-Enhanced-Trilogy"
DATA = [
    "decompiler/config",
    "goal_src",
    "game/assets",
    "game/graphics/opengl_renderer/shaders",
    "custom_assets",
    "scripts/enhanced/hd_setup.py",  # the HD textures, made by the player (the launcher asks)
    "scripts/enhanced/hd_textures.py",
    "scripts/enhanced/minimap_maps.py",
]


def tracked(folder):
    out = subprocess.run(["git", "ls-files", "-z", folder], cwd=ROOT, check=True,
                         capture_output=True).stdout.decode()
    return [f for f in out.split("\0") if f]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin", type=Path, default=ROOT / "out/build/Release-static/bin")
    parser.add_argument("--version", default="")
    parser.add_argument("--out", type=Path, default=ROOT / "out/release")
    args = parser.parse_args()

    dest = args.out / NAME
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for exe in ("gk.exe", "goalc.exe", "extractor.exe", "decompiler.exe"):
        shutil.copy2(args.bin / exe, dest / exe)

    count = 0
    for folder in DATA:
        for rel in tracked(folder):
            target = dest / "data" / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / rel, target)
            count += 1
    (dest / "data/launcher").mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / ".github/scripts/releases/error-code-metadata.json",
                 dest / "data/launcher/error-code-metadata.json")
    (dest / "data/log").mkdir(exist_ok=True)
    # the launcher compiles the games again when this changes (an update over an old folder)
    rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True,
                         text=True).stdout.strip()
    (dest / "data/VERSION").write_text(f"{args.version or 'dev'}-{rev}", encoding="ascii")

    shutil.copy2(ROOT / "Jak Enhanced Trilogy.bat", dest / "Jak Enhanced Trilogy.bat")
    (dest / "ISO").mkdir()
    shutil.copy2(ROOT / "ISO/PUT-YOUR-ISO-FILES-HERE.txt", dest / "ISO/PUT-YOUR-ISO-FILES-HERE.txt")
    (dest / "README.txt").write_text(
        "Jak Enhanced Trilogy " + args.version + "\r\n\r\n"
        "1. Put your own disc images (.iso) in the ISO folder: Jak and Daxter: The Precursor Legacy,\r\n"
        "   Jak II, Jak 3 (one, two or all three).\r\n"
        "2. Double-click \"Jak Enhanced Trilogy.bat\".\r\n\r\n"
        "The first time you play a game it extracts your disc (20-40 minutes, only once).\r\n"
        "In the game: F1 (or \\, or Select + R3 on a pad) opens the Enhanced menu, F9 switches to\r\n"
        "the next game.\r\n\r\n"
        "Free and unofficial: no donations of any kind are accepted for this project.\r\n\r\n"
        "https://github.com/stichlola/jak-enhanced-trilogy\r\n", encoding="ascii")

    zip_path = args.out / f"{NAME}{'-' + args.version if args.version else ''}.zip"
    zip_path.unlink(missing_ok=True)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(dest.rglob("*")):
            z.write(f, Path(NAME) / f.relative_to(dest))
    print(f"{count} data files; {zip_path} ({zip_path.stat().st_size / 1e6:.0f} MB)")


if __name__ == "__main__":
    sys.exit(main())
