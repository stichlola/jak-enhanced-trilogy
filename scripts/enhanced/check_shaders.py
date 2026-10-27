"""Jak Enhanced Trilogy: every shader checked by Khronos' reference compiler, as strict as AMD's driver.

    python scripts/enhanced/check_shaders.py [path/to/glslang.exe]

NVIDIA's driver accepts GLSL the spec doesn't; AMD's refuses it, and the shader is then missing in
the game. This prepares each pair the way Shader.cpp does (the HEIGHT_SCALE / SCISSOR_ macros, the
cut volumes' and the TAA jitter's wrapped main()) and has glslang compile and link it (glslang from
https://github.com/KhronosGroup/glslang/releases, default local/tools/glslang/x/bin). Also flags any
byte that isn't ASCII (AMD refuses those, glslang doesn't). Prints the errors, exit code 1 if any.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "game/graphics/opengl_renderer/shaders"
CUT = {"tfrag3", "shrub", "etie", "etie_base"}


def prepare(name, vert, frag):
    vert = vert.replace("HEIGHT_SCALE", "1.0").replace("SCISSOR_HEIGHT", "416.0")
    frag = frag.replace("SCISSOR_HEIGHT", "416.0")
    vert = vert.replace("SCISSOR_ADJUST", "(512.0 / 416.0)")
    main = re.compile(r"void\s+main\s*\(\s*\)")
    if name in CUT:
        # (the same text as Shader.cpp's, enough to compile)
        vert = main.sub("void enh_cut_main()", vert)
        vert += "\nout vec3 enh_world;\nvoid main() {\n  enh_world = position_in / 4096.0;\n  enh_cut_main();\n}\n"
        frag = main.sub("void enh_cut_frag_main()", frag)
        frag += ("\nin vec3 enh_world;\nuniform int enh_cut;\nuniform int enh_grade_on;\nuniform vec4 enh_grade;\n"
                 "void main() {\n  if (enh_cut != 0 && enh_world.x > 0.0) discard;\n  enh_cut_frag_main();\n"
                 "  if (enh_grade_on != 0) { color.rgb *= enh_grade.rgb; }\n}\n")
    # the TAA jitter (wrapped the same way on the shaders that have it)
    vert = main.sub("void enh_original_main()", vert)
    vert += ("\nuniform vec2 enh_jitter;\nvoid main() {\n  enh_original_main();\n"
             "  gl_Position.xy += enh_jitter * gl_Position.w;\n}\n")
    return vert, frag


def main():
    glslang = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "local/tools/glslang/x/bin/glslang.exe"
    bad = 0
    # AMD's driver takes ASCII only (a UTF-8 byte order mark failed enh_volume there); glslang
    # doesn't mind, so this is checked apart. Shader.cpp blanks such bytes anyway.
    for path in sorted(SHADERS.glob("*.*")):
        data = path.read_bytes()
        at = next((i for i, b in enumerate(data) if b >= 0x80), None)
        if at is not None:
            bad += 1
            line = data[:at].count(b"\n") + 1
            print(f"== {path.name}: non-ASCII byte 0x{data[at]:02X} on line {line}")
    if not glslang.exists():
        print(f"glslang not found ({glslang}): only the ASCII check ran")
        print(f"{bad} shader(s) with errors")
        sys.exit(1 if bad else 0)
    with tempfile.TemporaryDirectory() as tmp:
        for vpath in sorted(SHADERS.glob("*.vert")):
            name = vpath.stem
            fpath = SHADERS / f"{name}.frag"
            if not fpath.exists():
                continue
            vert, frag = prepare(name, vpath.read_text(encoding="utf-8"), fpath.read_text(encoding="utf-8"))
            v, f = Path(tmp) / f"{name}.vert", Path(tmp) / f"{name}.frag"
            v.write_text(vert, encoding="utf-8")
            f.write_text(frag, encoding="utf-8")
            r = subprocess.run([str(glslang), "-l", str(v), str(f)], capture_output=True, text=True)
            errors = [l for l in (r.stdout + r.stderr).splitlines() if "ERROR" in l]
            if errors:
                bad += 1
                print(f"== {name}")
                for l in errors[:8]:
                    print("  " + l.replace(tmp, ""))
    print(f"{bad} shader(s) with errors")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
