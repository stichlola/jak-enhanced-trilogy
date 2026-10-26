"""Jak Enhanced Trilogy: the optional HD textures pack, one ZIP a game, to share outside GitHub.

    python scripts/enhanced/make_hd_pack.py [--games jak1 jak2 jak3]

Takes custom_assets/<game>/texture_replacements (made by hd_textures.py) and lays it out like the
release: data/custom_assets/<game>/texture_replacements. Players unzip it into the Jak Enhanced
Trilogy folder; the launcher sees it and extracts that game's levels again, once.

The textures come from the games' own, so the packs never go in git or in the release ZIP.
"""

import argparse
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--games", nargs="+", default=["jak1", "jak2", "jak3"])
    parser.add_argument("--out", type=Path, default=ROOT / "out/release")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    for game in args.games:
        src = ROOT / "custom_assets" / game / "texture_replacements"
        if not src.is_dir():
            print(f"{game}: no {src}, skipped")
            continue
        zip_path = args.out / f"Jak-Enhanced-HD-Textures-{game}.zip"
        zip_path.unlink(missing_ok=True)
        files = sorted(f for f in src.rglob("*") if f.is_file())
        # PNGs are already compressed: storing them is as small and much faster
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_STORED) as z:
            for f in files:
                z.write(f, Path("data/custom_assets") / game / "texture_replacements" / f.relative_to(src))
        print(f"{game}: {len(files)} textures, {zip_path} ({zip_path.stat().st_size / 1e9:.1f} GB)")


if __name__ == "__main__":
    sys.exit(main())
