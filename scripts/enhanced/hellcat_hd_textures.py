"""Jak Enhanced: the textures of a ripped merc model (the Hellcat), at twice their size, for hellcat_hd.py.

    python scripts/enhanced/hellcat_hd_textures.py <ripped.glb> <out dir> [--gpu 1] [--max-size 2048]

Reads every image of the glb (the decompiler's rip, which already carries the 4x HD textures of
texture_replacements), upscales it 4x with Real-ESRGAN (wrap-padded, so tiling textures stay seamless),
brings it back down to 2x with Lanczos (sharper than a 2x upscale) and adds a faint periodic metal
grain, so the panels aren't smooth plastic at close range. The alpha is the original's, resized
smoothly (the upscaler would soften cutouts and the windshield), doubled: the rip has the PS2's range
(128 is opaque) and a replacement's textures are read in the PC's (255, halved at extraction).
Writes <out dir>/<name>.png. Nothing here is tracked by git: it is the game's art.
"""

import argparse
import base64
import io
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
PAD = 16


def find_esrgan():
    for exe in (ROOT / "out/tools/realesrgan/realesrgan-ncnn-vulkan.exe",
                ROOT.parent / "jak3-ue5/tools/realesrgan/bin/realesrgan-ncnn-vulkan.exe"):
        if exe.exists():
            return exe
    raise SystemExit("Real-ESRGAN not found: run scripts/enhanced/hd_setup.py")


def glb_images(path):
    """{name: RGBA PIL image} of a glb whose images are data URIs or buffer views"""
    data = Path(path).read_bytes()
    length = struct.unpack("<I", data[12:16])[0]
    gltf = json.loads(data[20:20 + length])
    binary = data[20 + length + 8:]
    images = {}
    for image in gltf.get("images", []):
        if "uri" in image:
            raw = base64.b64decode(image["uri"].split(",", 1)[1])
        else:
            view = gltf["bufferViews"][image["bufferView"]]
            raw = binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]]
        images.setdefault(image["name"], Image.open(io.BytesIO(raw)).convert("RGBA"))
    return images


def grain(w, h, seed, strength):
    """A periodic (tiles with the texture) fine noise around 1.0: brushed-metal-like streaks along x"""
    rng = np.random.default_rng(seed)
    noise = rng.standard_normal((h, w))
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    # (stretched along x: streaks, and only the fine scales so it reads as surface, not dirt)
    radius = np.sqrt((fx * 0.35) ** 2 + fy ** 2)
    band = np.exp(-((radius - 0.18) / 0.10) ** 2)
    noise = np.real(np.fft.ifft2(np.fft.fft2(noise) * band))
    noise /= noise.std() + 1e-9
    return 1.0 + strength * noise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("glb")
    parser.add_argument("out")
    parser.add_argument("--gpu", type=int)
    parser.add_argument("--max-size", type=int, default=2048)
    parser.add_argument("--grain", type=float, default=0.035)
    parser.add_argument("--model", default="realesrgan-x4plus")
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    images = glb_images(args.glb)
    esrgan = find_esrgan()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "in").mkdir()
        (tmp / "up").mkdir()
        for name, im in images.items():
            a = np.asarray(im)
            padded = np.pad(a, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="wrap")
            Image.fromarray(padded).convert("RGB").save(tmp / "in" / f"{name}.png")
        command = [str(esrgan), "-i", str(tmp / "in"), "-o", str(tmp / "up"), "-n", args.model, "-s", "4",
                   "-m", str(esrgan.parent / "models"), "-f", "png"]
        if args.gpu is not None:
            command += ["-g", str(args.gpu)]
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)

        for i, (name, im) in enumerate(sorted(images.items())):
            w, h = im.size
            scale = min(2.0, args.max_size / max(w, h))
            nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
            up = Image.open(tmp / "up" / f"{name}.png").convert("RGB")
            up = up.crop((PAD * 4, PAD * 4, PAD * 4 + w * 4, PAD * 4 + h * 4)).resize((nw, nh), Image.LANCZOS)
            up = up.filter(ImageFilter.UnsharpMask(radius=1.2, percent=40, threshold=2))
            rgb = np.asarray(up).astype(np.float32)
            rgb *= grain(nw, nh, i + 1, args.grain)[..., None]
            alpha = np.asarray(im.getchannel("A").resize((nw, nh), Image.BICUBIC)).astype(np.int32)
            alpha = np.clip(alpha * 2, 0, 255).astype(np.uint8)
            rgba = np.dstack([np.clip(rgb, 0, 255).astype(np.uint8), alpha])
            Image.fromarray(rgba, "RGBA").save(out / f"{name}.png")
            print(f"{name}: {w}x{h} -> {nw}x{nh}")


if __name__ == "__main__":
    sys.exit(main())
