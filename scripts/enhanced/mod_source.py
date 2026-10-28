"""Enhanced Light's mod source for OpenGOAL's launcher: mods.json at the repo's root, which players add
in the launcher (Settings > Mods > add a source) by its raw GitHub URL:

    https://raw.githubusercontent.com/stichlola/jak-enhanced-trilogy/main/mods.json

    python scripts/enhanced/mod_source.py --add v0.7   # a new version (newest first), after its release
    python scripts/enhanced/mod_source.py              # only rewrites the mod's texts and checks the file

The version's asset is the release's Jak-Enhanced-Light-<version>.zip (make_release.py --light), the
same ZIP for the three games. Checked against the launcher's schema (schemas/mod-source/v1).
"""

import argparse
import datetime
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REPO = "stichlola/jak-enhanced-trilogy"
RAW = f"https://raw.githubusercontent.com/{REPO}/main"
GAMES = ["jak1", "jak2", "jak3"]

MOD = {
    "displayName": "Jak Enhanced Light",
    "description": (
        "New lighting and graphics for the game as it is: real-time sun and shadows, ambient occlusion, "
        "reflections on water and wet streets, rain, fog, sandstorms and a night sky, HD pause maps, and "
        "an in-game menu to tune it all (F1). Gameplay is untouched. The single-game version of Jak "
        "Enhanced Trilogy (the full one, with the three games in one program, is on GitHub)."
    ),
    "authors": ["stichlola"],
    "tags": ["graphics", "visual", "weather"],
    "supportedGames": GAMES,
    "websiteUrl": f"https://github.com/{REPO}",
    "coverArtUrl": f"{RAW}/game/assets/enhanced/art/cover.jpg",
    "thumbnailArtUrl": f"{RAW}/game/assets/enhanced/art/banner-1280x640.png",
}


def check(data):
    """the launcher's own rules (mod-source-schema.v1.json), without a schema library"""
    allowed = {"displayName", "description", "authors", "tags", "supportedGames", "versions", "websiteUrl",
               "coverArtUrl", "thumbnailArtUrl", "externalLink", "perGameConfig"}
    assert re.fullmatch(r"\d+\.\d+\.\d+", data["schemaVersion"])
    assert set(data) == {"schemaVersion", "sourceName", "lastUpdated", "mods", "texturePacks"}
    for name, mod in data["mods"].items():
        assert set(mod) <= allowed, f"{name}: unknown keys {set(mod) - allowed}"
        for key in ("displayName", "description", "authors", "tags", "supportedGames", "versions"):
            assert key in mod, f"{name}: no {key}"
        assert set(mod["supportedGames"]) <= {"jak1", "jak2", "jak3", "jakx"}
        for v in mod["versions"]:
            assert set(v) <= {"version", "publishedDate", "assets", "supportedGames"}
            assert {"version", "publishedDate", "assets"} <= set(v)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--add", metavar="TAG", help="the release tag of a new version (v0.7)")
    args = parser.parse_args()

    path = ROOT / "mods.json"
    data = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    versions = data.get("mods", {}).get("jak-enhanced-light", {}).get("versions", [])
    if args.add:
        version = args.add.lstrip("v")
        versions = [v for v in versions if v["version"] != version]
        versions.insert(0, {
            "version": version,
            "publishedDate": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "assets": {"windows": f"https://github.com/{REPO}/releases/download/{args.add}/"
                                  f"Jak-Enhanced-Light-{args.add}.zip", "linux": None, "macos": None},
            "supportedGames": GAMES,
        })
    out = {
        "schemaVersion": "1.0.0",
        "sourceName": "Jak Enhanced",
        "lastUpdated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "mods": {"jak-enhanced-light": {**MOD, "versions": versions}},
        "texturePacks": {},
    }
    check(out)
    path.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"{path}: {len(versions)} version(s)" + (f", newest {versions[0]['version']}" if versions else ""))


if __name__ == "__main__":
    sys.exit(main())
