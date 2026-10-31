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

What goes in: the files tracked by git, so nothing from the games (extracted data, HD textures made
from the game's own, settings) can slip into it, plus HANDMADE: the folders made by hand with Tripo,
which stay out of git and are copied from the disk (the release stops if one is missing).

What a player can edit (SHIPPED: the main world) goes in data/shipped in the trilogy's release, with
a manifest of hashes, and the launcher puts it in place file by file (apply_shipped.ps1), keeping
what the player changed. In Enhanced Light it stays in its place: OpenGOAL's launcher deletes a mod's
folder at each version anyway.
"""

import argparse
import hashlib
import json
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
             "scripts/enhanced/minimap_maps.py", "scripts/enhanced/update.ps1",
             "scripts/enhanced/report.ps1", "scripts/enhanced/apply_shipped.ps1",
             "scripts/enhanced/refresh_models.ps1"}
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
    "scripts/enhanced/apply_shipped.ps1",  # the main world put in place, keeping the player's edits
    "scripts/enhanced/refresh_models.ps1",  # the levels holding new replacement models made again
    "scripts/enhanced/report.ps1",  # the report window (a crash opens it, the menu button too)
]
# Made by hand with Tripo, kept out of git (.gitignore), in every release: a folder and how many files
# it has to hold at least. Only what is known to be Tripo's or hand-made goes on this list: not
# texture_remade/level-default-pris (tools/ai_textures/repaint_jak.py computes it from the game's own
# textures), not scenery/game_* (taken from the disc by the editor).
HANDMADE = [
    ("custom_assets/jak3/texture_remade/_all", 1),  # tools/tripo/tasks.json in jak3-ue5: tex-*, tex2-*
    ("custom_assets/jak3/merc_replacements", 2),  # the same file: spargus_market
]
# the same, by name in a folder shared with what git tracks: the editor's Tripo kits (rocks, slabs,
# pebbles, trees, with their far versions), in its library even where the main world doesn't use them
HANDMADE_FILES = ["custom_assets/jak3/scenery/desert_*", "custom_assets/jak3/scenery/rock_big_*",
                  "custom_assets/jak3/scenery/tree_*"]
# What the player can edit in the game (F4): placed_path, manual_lights_path, grass_paint, kTerrain
# (the terrain's pictures, which the Terrain window replaces) in the source
SHIPPED = [
    "game/assets/enhanced/placed.json",
    "game/assets/enhanced/lights.json",
    "game/assets/enhanced/grass_paint",
    "custom_assets/jak3/scenery/rock_detail.png",
    "custom_assets/jak3/scenery/ground_detail.png",
    "custom_assets/jak3/scenery/sand_fine.png",
    "custom_assets/jak3/scenery/sand_coarse.png",
    "custom_assets/jak3/scenery/sand_crust.png",
]
# the sources of the programs: a program older than these isn't what the commit says
PROGRAM_SOURCES = ["game", "common", "decompiler", "goalc"]


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True).stdout


def tracked(folder):
    return [f for f in git("ls-files", "-z", folder).decode().split("\0") if f]


def warn_if_stale(folders, bin_dir):
    """What's going in that isn't the commit: this copies the working tree, not HEAD."""
    dirty = [line for line in git("status", "--porcelain", "--", *folders, *PROGRAM_SOURCES).decode().splitlines()
             if not line.startswith("??")]
    if dirty:
        print(f"WARNING: {len(dirty)} files with uncommitted changes (the release takes them as they are now):")
        for line in dirty[:30]:
            print("   ", line)
    last = int(git("log", "-1", "--format=%ct", "--", *PROGRAM_SOURCES).decode().strip())
    for exe in ("gk.exe", "goalc.exe", "extractor.exe", "decompiler.exe"):
        if (bin_dir / exe).stat().st_mtime < last:
            print(f"WARNING: {bin_dir / exe} is older than the last commit to its sources: build it again")


def copy_handmade(dest):
    """Fails loudly: a release without them would look like one, and lack the models."""
    count = 0
    for folder, at_least in HANDMADE:
        source = ROOT / folder
        files = sorted(f for f in source.rglob("*") if f.is_file()) if source.is_dir() else []
        if len(files) < at_least:
            sys.exit(f"{folder}: {len(files)} files, at least {at_least} expected. Stopped: this release "
                     "would miss what's made by hand")
        taken = sum(f.stat().st_size for f in files)
        for f in files:
            target = dest / "data" / f.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, target)
            count += 1
        print(f"{folder}: {len(files)} files, {taken / 1e6:.1f} MB")
    for pattern in HANDMADE_FILES:
        files = sorted(f for f in ROOT.glob(pattern) if f.is_file() and not f.name.endswith("_preview.png"))
        if not files:
            sys.exit(f"{pattern}: no files. Stopped: this release would miss what's made by hand")
        for f in files:
            target = dest / "data" / f.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, target)
            count += 1
        print(f"{pattern}: {len(files)} files, {sum(f.stat().st_size for f in files) / 1e6:.1f} MB")
    return count


def drop_missing_models(dest):
    """Objects of the main world whose model doesn't ship (the editor's imports from the disc, game_*
    in .gitignore) would be nothing but a warning in the player's log."""
    path = dest / "data/game/assets/enhanced/placed.json"
    placed = json.loads(path.read_text(encoding="utf-8"))
    for game, objects in placed.items():
        scenery = dest / "data/custom_assets" / game / "scenery"
        missing = sorted({o["model"] for o in objects if not (scenery / o["model"]).exists()})
        if missing:
            print(f"WARNING: placed.json ({game}): left out objects with no model in the release: "
                  + ", ".join(f"{m} ({sum(o['model'] == m for o in objects)})" for m in missing))
            placed[game] = [o for o in objects if o["model"] not in missing]
    path.write_text(json.dumps(placed, indent=1), encoding="utf-8", newline="\n")


def file_key(rel, data):
    """As apply_shipped.ps1 compares them: line ends alike in text."""
    if rel.endswith(".json"):
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def earlier_keys(rel):
    """Every version the file ever had in git (each release's is among them)."""
    keys = set()
    for commit in git("log", "--format=%H", "--", rel).decode().split():
        show = subprocess.run(["git", "show", f"{commit}:{rel}"], cwd=ROOT, capture_output=True)
        if show.returncode == 0:
            keys.add(file_key(rel, show.stdout))
    return keys


def move_to_shipped(dest, version):
    """data/shipped/<path> and its manifest (see apply_shipped.ps1)."""
    data = dest / "data"
    shipped = data / "shipped"
    files = {}
    for item in SHIPPED:
        place = data / item
        for f in sorted(place.rglob("*")) if place.is_dir() else [place]:
            if not f.is_file():
                continue
            rel = f.relative_to(data).as_posix()
            key = file_key(rel, f.read_bytes())
            target = shipped / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(f), target)
            files[rel] = {"sha256": key, "previous": sorted(earlier_keys(rel) - {key})}
        if place.is_dir():
            shutil.rmtree(place)
    (shipped / "manifest.json").write_text(json.dumps({"version": version, "files": files}, indent=1),
                                           encoding="utf-8", newline="\n")
    print(f"data/shipped: {len(files)} files the player can edit")


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

    folders = [d for d in DATA if not (args.light and d in FULL_ONLY)]
    warn_if_stale(folders, args.bin)

    name = LIGHT_NAME if args.light else NAME
    dest = args.out / name
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for exe in ("gk.exe", "goalc.exe", "extractor.exe", "decompiler.exe"):
        shutil.copy2(args.bin / exe, dest / exe)

    count = 0
    for folder in folders:
        for rel in tracked(folder):
            target = dest / "data" / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / rel, target)
            count += 1
    count += copy_handmade(dest)
    drop_missing_models(dest)
    (dest / "data/launcher").mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / ".github/scripts/releases/error-code-metadata.json",
                 dest / "data/launcher/error-code-metadata.json")
    (dest / "data/log").mkdir(exist_ok=True)
    # the launcher compiles the games again when this changes (an update over an old folder)
    rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True,
                         text=True).stdout.strip()
    version = f"{args.version or 'dev'}-{rev}"
    (dest / "data/VERSION").write_text(version, encoding="ascii")

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

    move_to_shipped(dest, version)
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
        "Something wrong? If the game crashes, a report window opens by itself: one click sends it.\r\n"
        "Feedback any time: \"Send feedback\" in the Enhanced menu, or \"Report a problem.bat\".\r\n\r\n"
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
