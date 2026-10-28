"""Jak Enhanced Trilogy: the release ZIP for players (nothing to build or install).

    python scripts/enhanced/make_release.py [--bin out/build/Release-static/bin] [--version v1.0] [--installer]
    python scripts/enhanced/make_release.py --light [--version v1.0]

--light makes Enhanced Light instead, the mod for OpenGOAL's launcher (mods.json): the same programs
and data/ at the ZIP's root (the launcher unpacks it into its own mod folder and runs the extractor
itself), with data/enhanced-light switching the single-game build on (enhanced_light.h), and none of
the trilogy's own launcher, report and update scripts.

Lays out what OpenGOAL's own releases have (.github/scripts/releases/extract_build_windows.sh):
gk.exe, goalc.exe and extractor.exe (the statically linked build: no runtime DLLs), and next to them
data/ with the decompiler's config, the game code, the assets, the shaders and the custom levels; the
programs find data/ by themselves. Plus the one-click launcher and the ISO folder.

Only files tracked by git go in, so nothing from the games (extracted data, HD textures made from
the game's own, settings) can slip into it.
"""

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAME = "Jak-Enhanced-Trilogy"
LIGHT_NAME = "Jak-Enhanced-Light"
# (not in Enhanced Light: the launcher there is OpenGOAL's, and the HD textures need Python)
FULL_ONLY = {"scripts/enhanced/hd_setup.py", "scripts/enhanced/hd_textures.py",
             "scripts/enhanced/minimap_maps.py", "scripts/enhanced/update.ps1"}
DATA = [
    "decompiler/config",
    "goal_src",
    "game/assets",
    "game/graphics/opengl_renderer/shaders",
    "custom_assets",
    "scripts/enhanced/hd_setup.py",  # the HD textures, made by the player (the launcher asks)
    "scripts/enhanced/hd_textures.py",
    "scripts/enhanced/minimap_maps.py",
    "scripts/enhanced/update.ps1",  # a new release installed over this one (the launcher asks)
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
    parser.add_argument("--installer", action="store_true", help="also the setup .exe (Inno Setup 6)")
    parser.add_argument("--light", action="store_true", help="Enhanced Light, the OpenGOAL launcher mod")
    parser.add_argument("--skip-shader-check", action="store_true",
                        help="don't stop on shader errors (check_shaders.py: glslang, ASCII only)")
    args = parser.parse_args()

    # every shader compiled by Khronos' reference compiler and ASCII only, as AMD's driver wants them
    # (NVIDIA's lets through what AMD refuses: a release once wouldn't start on AMD cards)
    if not args.skip_shader_check:
        check = subprocess.run([sys.executable, str(ROOT / "scripts/enhanced/check_shaders.py")], cwd=ROOT)
        if check.returncode != 0:
            sys.exit("shader check failed (see above); --skip-shader-check to package anyway")

    name = LIGHT_NAME if args.light else NAME
    dest = args.out / name
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for exe in ("gk.exe", "goalc.exe", "extractor.exe", "decompiler.exe"):
        shutil.copy2(args.bin / exe, dest / exe)

    count = 0
    for folder in [d for d in DATA if not (args.light and d in FULL_ONLY)]:
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

    if args.light:
        (dest / "data/enhanced-light").write_text(
            "Enhanced Light: the single-game build (common/util/enhanced_light.h)\n", encoding="ascii")
        zip_path = args.out / f"{LIGHT_NAME}{'-' + args.version if args.version else ''}.zip"
        zip_path.unlink(missing_ok=True)
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(dest.rglob("*")):
                # (no top folder: the launcher wants gk.exe and data/ at the root)
                z.write(f, f.relative_to(dest))
        print(f"{count} data files; {zip_path} ({zip_path.stat().st_size / 1e6:.0f} MB)")
        return

    for bat in ("Jak Enhanced Trilogy.bat", "Report a problem.bat"):
        # CRLF whatever the checkout has (cmd's labels and goto break with LF)
        text = (ROOT / bat).read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        (dest / bat).write_bytes(text)
    (dest / "ISO").mkdir()
    shutil.copy2(ROOT / "ISO/PUT-YOUR-ISO-FILES-HERE.txt", dest / "ISO/PUT-YOUR-ISO-FILES-HERE.txt")
    (dest / "README.txt").write_text(
        "Jak Enhanced Trilogy " + args.version + "\r\n\r\n"
        "1. Put your own disc images (.iso) in the ISO folder: Jak and Daxter: The Precursor Legacy,\r\n"
        "   Jak II, Jak 3 (one, two or all three).\r\n"
        "2. Double-click \"Jak Enhanced Trilogy.bat\".\r\n\r\n"
        "The first time you play a game it extracts your disc (20-40 minutes, only once), then asks\r\n"
        "whether to make the HD textures from it (optional: 30 minutes to 2 hours a game).\r\n"
        "In the game: F1 (or \\, or Select + R3 on a pad) opens the Enhanced menu; Select (View on an\r\n"
        "Xbox pad) or N opens the nap window: Jak sleeps the hours away while the world runs fast.\r\n\r\n"
        "Something wrong? Double-click \"Report a problem.bat\": it puts your newest logs, graphics card,\r\n"
        "driver and settings in a ZIP on your Desktop to attach to a message (nothing is sent by itself).\r\n\r\n"
        "Free and unofficial: no donations of any kind are accepted for this project.\r\n\r\n"
        "https://github.com/stichlola/jak-enhanced-trilogy\r\n", encoding="ascii")

    zip_path = args.out / f"{NAME}{'-' + args.version if args.version else ''}.zip"
    zip_path.unlink(missing_ok=True)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(dest.rglob("*")):
            z.write(f, Path(NAME) / f.relative_to(dest))
    print(f"{count} data files; {zip_path} ({zip_path.stat().st_size / 1e6:.0f} MB)")

    if args.installer:
        # Setup-Jak-Enhanced-Trilogy-<version>.exe from the same folder (installer.iss)
        iscc = Path(os.environ.get("LOCALAPPDATA", "")) / "Programs/Inno Setup 6/ISCC.exe"
        if not iscc.exists():
            iscc = Path(os.environ.get("ProgramFiles(x86)", "")) / "Inno Setup 6/ISCC.exe"
        subprocess.run([str(iscc), "/Q", f"/DMyVersion={args.version or 'dev'}", f"/DSrc={dest}",
                        f"/O{args.out}", str(ROOT / "scripts/enhanced/installer.iss")], check=True)
        for exe in args.out.glob("Setup-*.exe"):
            print(f"{exe} ({exe.stat().st_size / 1e6:.0f} MB)")


if __name__ == "__main__":
    sys.exit(main())
