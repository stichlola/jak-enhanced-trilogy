"""Jak 3 Enhanced: the desert dunes' sand from a seamless picture instead of the Wang tile atlas.

    python scripts/enhanced/desert_sand.py [TILE.png] [--tint R,G,B]

Takes a seamless sand tile (default: the Tripo sand made for the jak3-ue5 Unreal version,
tools/tripo/raw/desert_sand_tile.png), brings its average color to the game's own sand (the
mean of the Wang atlas desert-hfrag/wang_0 that the montage really draws; its mip is much brighter
and yellower, the color Unreal needed but here it saturates) with PS2 alpha (0x80 = opaque)
and writes out/enhanced/desert_sand.png. The hfrag renderer picks it up at startup and samples it
in world space, repeating every 64 m; without the file the game draws its own Wang tiles.

--tint scales the result further (default 1,1,1).
"""

import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UE_TOOLS = ROOT.parent / "jak3-ue5/tools"
sys.path.insert(0, str(UE_TOOLS / "glb"))
from hfrag_to_glb import read_png  # noqa: E402  (pure-python PNG reader: width, height, RGBA)

ATLAS = ROOT / "decompiler_out/jak3_pal/textures/desert-hfrag/wang_0.png"
OUT = ROOT / "out/enhanced/desert_sand.png"


def write_png(path, width, height, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    rows = b"".join(b"\0" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    args = sys.argv[1:]
    tint = [1.0, 1.0, 1.0]
    if "--tint" in args:
        i = args.index("--tint")
        tint = [float(v) for v in args[i + 1].split(",")]
        del args[i:i + 2]
    tile_path = Path(args[0]) if args else UE_TOOLS / "tripo/raw/desert_sand_tile.png"
    width, height, tile = read_png(tile_path)
    _, _, atlas = read_png(ATLAS)
    tile_mean = [sum(tile[i::4]) / (width * height) for i in range(3)]
    atlas_mean = [sum(atlas[i::4]) / (len(atlas) // 4) for i in range(3)]
    gain = [m / t * k for m, t, k in zip(atlas_mean, tile_mean, tint)]
    sand = bytearray(tile)
    for i in range(0, len(sand), 4):
        for ch in range(3):
            sand[i + ch] = min(255, round(sand[i + ch] * gain[ch]))
        sand[i + 3] = 128
    OUT.parent.mkdir(parents=True, exist_ok=True)
    write_png(OUT, width, height, sand)
    print(f"{OUT}: {width}x{height}, gain {[round(g, 3) for g in gain]}")


if __name__ == "__main__":
    sys.exit(main())
