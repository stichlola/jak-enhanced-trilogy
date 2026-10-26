"""Enhanced on each game: what's hooked up and what's missing.

For Jak 1, 2 and 3 it checks the three layers an Enhanced feature needs on a game:
  1. the renderer: EnhancedPost hooked into the game's bucket dispatch (OpenGLRenderer.cpp)
  2. the GOAL bridges: the game's code telling the renderer the sun, the scene, Jak, ...
     (goal_src/<game>, the pc-enhanced-* kernel functions)
  3. the data: game/assets/enhanced/world.json (tuning, the zones of the game's levels)
and lists the game's levels that are in no zone yet (they get the neutral look and no weather).

  python scripts/enhanced/check_games.py [jak1|jak2|jak3]
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAMES = ["jak1", "jak2", "jak3"]

# what the renderer needs in dispatch_buckets_<game>
RENDER_HOOKS = {
    "post effects on the scene": "apply_to_scene",
    "near ocean (reflections)": "begin_ocean",
    "far ocean": "capture_far_ocean",
}
# the GOAL side: (what, kernel function or env var, required)
BRIDGES = [
    ("sun direction", "pc-enhanced-set-sun", True),
    ("level, hour, cutscene", "pc-enhanced-set-scene", True),
    ("Jak (grass, footprints)", "pc-enhanced-set-player", True),
    ("who pushes the grass", "pc-enhanced-set-pushers", True),
    ("in-game menu requests", "pc-enhanced-poll-request", True),
    ("thunder after the bolts", "pc-enhanced-thunder", True),
    ("test boot point", "JAK_ENHANCED_START", True),
    ("test hour", "JAK_ENHANCED_HOUR", True),
    ("lamps", "pc-enhanced-set-lights", False),
    ("explosions", "pc-enhanced-explosion", False),
]


def load_world():
    text = (ROOT / "game/assets/enhanced/world.json").read_text(encoding="utf-8")
    # comments: whole-line and trailing // (no value in the file contains //)
    text = "\n".join(re.sub(r"\s*//.*$", "", line) for line in text.splitlines())
    return json.loads(text)


def dispatch_body(game):
    src = (ROOT / "game/graphics/opengl_renderer/OpenGLRenderer.cpp").read_text(encoding="utf-8")
    start = src.find(f"void OpenGLRenderer::dispatch_buckets_{game}(")
    if start < 0:
        return ""
    end = src.find("\nvoid OpenGLRenderer::", start + 1)
    return src[start:end]


def goal_text(game):
    parts = []
    for f in (ROOT / "goal_src" / game).rglob("*.gc"):
        parts.append(f.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(parts)


def game_levels(game):
    """The game's level names, from level-info.gc (the level-load-info definitions)."""
    text = (ROOT / "goal_src" / game / "engine/level/level-info.gc").read_text(encoding="utf-8")
    names = []
    for chunk in text.split("'level-load-info")[1:]:
        m = re.search(r":name '([a-z0-9-]+)", chunk)
        if m and m.group(1) not in names:
            names.append(m.group(1))
    return names


def check(game, world):
    print(f"== {game}")
    ok = True
    body = dispatch_body(game)
    for what, needle in RENDER_HOOKS.items():
        has = needle in body
        ok &= has
        print(f"  renderer  {'ok  ' if has else 'MISSING'}  {what} ({needle})")
    goal = goal_text(game)
    for what, needle, required in BRIDGES:
        # a call, not just the extern
        uses = len(re.findall(re.escape(needle), goal)) - len(re.findall(r"define-extern " + re.escape(needle), goal))
        has = uses > 0
        if required:
            ok &= has
        mark = "ok  " if has else ("MISSING" if required else "-   ")
        print(f"  goal      {mark}  {what} ({needle})")

    zones = world.get("zones", {})
    tuning = world.get("tuning", {}).get(game)
    print(f"  data      {'ok  ' if tuning else 'MISSING'}  tuning (gi {tuning and tuning.get('gi')}, lamps {tuning and tuning.get('lamps')})")
    ok &= bool(tuning)
    entries = world.get("levels", {}).get(game, [])
    prefixes = []
    for e in entries:
        if e["zone"] not in zones:
            ok = False
            print(f"  data      MISSING  zone '{e['zone']}' (used by {game}'s levels) is not in \"zones\"")
        prefixes += [(p, e["zone"]) for p in e["prefixes"]]
    levels = game_levels(game)
    by_zone, unzoned = {}, []
    for lev in levels:
        z = next((zone for p, zone in prefixes if lev.startswith(p)), None)
        if z:
            by_zone.setdefault(z, []).append(lev)
        else:
            unzoned.append(lev)
    print(f"  data      {len(levels) - len(unzoned)}/{len(levels)} levels in a zone")
    for z, levs in sorted(by_zone.items()):
        print(f"              {z}: {len(levs)}")
    if unzoned:
        print(f"  levels in no zone ({len(unzoned)}): {' '.join(unzoned)}")
    print(f"  => {'ready' if ok else 'something missing'}")
    return ok


def main():
    games = [a for a in sys.argv[1:] if a in GAMES] or GAMES
    world = load_world()
    results = [check(g, world) for g in games]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
