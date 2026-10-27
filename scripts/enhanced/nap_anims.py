"""Jak Enhanced Trilogy: Jak's nap animations, retargeted from Mixamo clips onto Jak 3's skeleton.

    blender -b -P scripts/enhanced/nap_anims.py -- <jakb-lod0.glb> <mixamo fbx folder> <out.glb>

jakb-lod0.glb is the decompiler's export of Jak 3's Jak (decompiler_out/jak3/levels/common, with
"rip_levels"); Jak 1's is eichar-lod0.glb, and the animations are then named eichar-nap-... (the
prefix is the glb's own, before "-lod0"). The Mixamo
clips (X Bot, FBX, 30 fps, no skin) are downloaded by hand from mixamo.com: getting_up_back,
pose_hands_behind_head_crossed and yawn. The output holds Jak's skeleton (without the "align"
joint, which build-actor adds itself), a one-triangle mesh and four animations, for
(build-actor "jak-nap" :master-art-group "jakb-ag") in game.gp (Jak 1: "eichar-ag"):

    jakb-nap-lie     lying down on his back (getting up, played backwards and faster)
    jakb-nap-sleep   asleep with his hands behind his head, a big cartoon breath (loops)
    jakb-nap-getup   getting up again, faster
    jakb-nap-stretch a yawn and a stretch, the arms pushed further

Retargeting: both skeletons stand in a T-pose facing the same way, so each Jak joint takes its
Mixamo joint's rotation relative to the rest pose (world space); joints without a match follow
their parent. The pelvis ("main") also takes the hips' height and travel. Everything is worked out
in glTF space from Jak's own joint transforms, so Blender's bone axes never come into it.
"""

import json
import math
import struct
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Quaternion, Vector

FPS = 30.0
# Jak joint <- Mixamo joint (prefix "mixamorig:")
MAP = {
    "main": "Hips", "hips": "Hips", "waist_prog": "Spine", "upper_body": "Spine1", "chest": "Spine2",
    "neckA": "Neck", "neckB": "Head",
}
for side, s in (("L", "Left"), ("R", "Right")):
    low = side.lower()
    MAP.update({
        side + "should": s + "Shoulder", side + "arm": s + "Arm", side + "forarm": s + "ForeArm",
        "sk_%shand" % low: s + "Hand",
        low + "thumA": s + "HandThumb1", low + "thumB": s + "HandThumb2",
        low + "indA": s + "HandIndex1", low + "indB": s + "HandIndex2",
        low + "midA": s + "HandMiddle1", low + "midB": s + "HandMiddle2",
        low + "ringA": s + "HandRing1", low + "ringB": s + "HandRing2",
        low + "pinkA": s + "HandPinky1", low + "pinkB": s + "HandPinky2",
        side + "thigh": s + "UpLeg", side + "knee": s + "Leg", side + "ankle": s + "Foot",
        side + "ball": s + "ToeBase",
    })
STAND_HEIGHT = 1.028  # main's height in Jak's own standing animations (glTF Y, meters): read from
                      # <prefix>-stance-loop in jak_skeleton()

# glTF (Y up) <-> Blender (Z up)
C = Matrix.Rotation(math.radians(90), 4, "X")
CI = C.inverted()


def read_glb(path):
    b = Path(path).read_bytes()
    n = struct.unpack("<I", b[12:16])[0]
    j = json.loads(b[20:20 + n])
    return j, b[20 + n + 8:]


def node_matrix(nd):
    if "matrix" in nd:
        m = nd["matrix"]
        return Matrix([m[0:4], m[4:8], m[8:12], m[12:16]]).transposed()
    t = Vector(nd.get("translation", (0, 0, 0)))
    r = nd.get("rotation", (0, 0, 0, 1))
    s = nd.get("scale", (1, 1, 1))
    return Matrix.LocRotScale(t, Quaternion((r[3], r[0], r[1], r[2])), Vector(s))


def jak_skeleton(glb):
    """Jak's joints (prejoint first) with their rest local matrices, glTF space; main unscaled, at
    the standing height (the bind pose is drawn 1.4x bigger)"""
    global STAND_HEIGHT
    j, binary = read_glb(glb)
    nodes = j["nodes"]
    prefix = Path(glb).name.split("-lod0")[0]
    for an in j.get("animations", []):
        if an["name"] != prefix + "-stance-loop":
            continue
        for ch in an["channels"]:
            if nodes[ch["target"]["node"]].get("name") == "main" and ch["target"]["path"] == "translation":
                acc = j["accessors"][an["samplers"][ch["sampler"]]["output"]]
                view = j["bufferViews"][acc["bufferView"]]
                at = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
                STAND_HEIGHT = struct.unpack("<3f", binary[at:at + 12])[1]
    joints = j["skins"][0]["joints"]
    names = [nodes[i]["name"] for i in joints]
    parent = {}
    for i in joints:
        for c in nodes[i].get("children", []):
            parent[c] = i
    out = []
    for i in joints[1:]:  # without align
        local = node_matrix(nodes[i])
        name = nodes[i]["name"]
        if name == "main":
            t, r, _ = local.decompose()
            local = Matrix.LocRotScale(Vector((t.x, STAND_HEIGHT, t.z)), r, Vector((1, 1, 1)))
        p = parent.get(i)
        out.append({"name": name, "parent": names.index(nodes[p]["name"]) - 1 if p in joints[1:] else -1,
                    "local": local})
    return out


def sample_mixamo(fbx):
    """Every frame's world matrices (Blender space, unit scale) of the Mixamo joints, and the rest"""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(fbx))
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    first, last = (int(x) for x in arm.animation_data.action.frame_range)

    def norm(m):
        t, r, _ = m.decompose()
        return Matrix.LocRotScale(t, r, Vector((1, 1, 1)))

    rest = {b.name.split(":")[-1]: norm(arm.matrix_world @ b.matrix_local) for b in arm.data.bones}
    frames = []
    for f in range(first, last + 1):
        bpy.context.scene.frame_set(f)
        frames.append({pb.name.split(":")[-1]: norm(arm.matrix_world @ pb.matrix) for pb in arm.pose.bones})
    return rest, frames


def retarget(skel, rest, frames, tweak=None):
    """Jak's local joint matrices (glTF) for every frame"""
    world_rest = []
    for jn in skel:
        w = jn["local"] if jn["parent"] < 0 else world_rest[jn["parent"]] @ jn["local"]
        world_rest.append(w)
    # Blender-space rest of Jak's joints
    brest = [C @ w @ CI for w in world_rest]
    hips0 = rest["Hips"].translation
    scale = STAND_HEIGHT / hips0.z
    out = []
    for fi, fr in enumerate(frames):
        deltas = []
        for k, jn in enumerate(skel):
            src = MAP.get(jn["name"])
            pd = deltas[jn["parent"]] if jn["parent"] >= 0 else Matrix.Identity(4)
            p_rest = brest[k].translation
            if src and src in fr:
                rot = (fr[src].to_quaternion() @ rest[src].to_quaternion().inverted())
                if tweak:
                    rot = tweak(jn["name"], rot, fi, len(frames))
                if jn["name"] == "main":
                    hp = fr["Hips"].translation
                    pos = Vector(((hp.x - hips0.x) * scale, (hp.y - hips0.y) * scale, hp.z * scale))
                else:
                    pos = pd @ p_rest
                d = Matrix.Translation(pos) @ rot.to_matrix().to_4x4() @ Matrix.Translation(-p_rest)
            else:
                d = pd
            deltas.append(d)
        worlds = [CI @ (deltas[k] @ brest[k]) @ C for k in range(len(skel))]
        out.append([worlds[k] if jn["parent"] < 0 else worlds[jn["parent"]].inverted() @ worlds[k]
                    for k, jn in enumerate(skel)])
    return out


def resample(frames, speed, reverse=False):
    """Plays the clip faster (and backwards): picks (and blends) frames at the new times"""
    src = frames[::-1] if reverse else frames
    n = max(2, int((len(src) - 1) / speed) + 1)
    out = []
    for i in range(n):
        x = min(i * speed, len(src) - 1)
        a, f = int(x), x - int(x)
        b = min(a + 1, len(src) - 1)
        out.append([lerp_matrix(ma, mb, f) for ma, mb in zip(src[a], src[b])])
    return out


def lerp_matrix(a, b, f):
    if f <= 0.0:
        return a
    ta, ra, sa = a.decompose()
    tb, rb, sb = b.decompose()
    return Matrix.LocRotScale(ta.lerp(tb, f), ra.slerp(rb, f), sa.lerp(sb, f))


def shift(frames, skel, at):
    """Moves the clip across the ground so main stands at (x, z) = at on its first frame"""
    m = [k for k, jn in enumerate(skel) if jn["name"] == "main"][0]
    t0 = frames[0][m].translation
    dx, dz = at[0] - t0.x, at[1] - t0.z
    for fr in frames:
        t, r, s = fr[m].decompose()
        fr[m] = Matrix.LocRotScale(Vector((t.x + dx, t.y, t.z + dz)), r, s)
    return frames


def write_glb(path, glb_src, skel, anims):
    j, binsrc = read_glb(glb_src)
    blob = bytearray()
    views, accs = [], []

    def add(data, kind, count, extra=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob.extend(data)
        a = {"bufferView": len(views) - 1, "componentType": 5126, "count": count, "type": kind}
        if extra:
            a.update(extra)
        accs.append(a)
        return len(accs) - 1

    # nodes: 0 the mesh, 1.. the joints (prejoint first)
    nodes = [{"name": "jak-nap", "mesh": 0, "skin": 0}]
    for k, jn in enumerate(skel):
        t, r, s = jn["local"].decompose()
        nd = {"name": jn["name"], "translation": list(t), "rotation": [r.x, r.y, r.z, r.w], "scale": list(s)}
        kids = [c + 1 for c, o in enumerate(skel) if o["parent"] == k]
        if kids:
            nd["children"] = kids
        nodes.append(nd)
    world = []
    for jn in skel:
        world.append(jn["local"] if jn["parent"] < 0 else world[jn["parent"]] @ jn["local"])
    ibm = b"".join(struct.pack("<16f", *[w.inverted()[r][c] for c in range(4) for r in range(4)])
                   for w in world)
    ibm_acc = add(ibm, "MAT4", len(skel))
    # a single tiny triangle on main (build-actor wants a skinned mesh; Jak's real one stays his own)
    pos = add(struct.pack("<9f", 0, 1, 0, 0.01, 1, 0, 0, 1.01, 0), "VEC3", 3,
              {"min": [0, 1, 0], "max": [0.01, 1.01, 0]})
    nrm = add(struct.pack("<9f", 0, 0, 1, 0, 0, 1, 0, 0, 1), "VEC3", 3)
    uv = add(struct.pack("<6f", 0, 0, 1, 0, 0, 1), "VEC2", 3)
    while len(blob) % 4:
        blob.append(0)
    views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": 12})
    blob.extend(bytes([1, 0, 0, 0] * 3))
    accs.append({"bufferView": len(views) - 1, "componentType": 5121, "count": 3, "type": "VEC4"})
    jnt = len(accs) - 1
    wgt = add(struct.pack("<12f", *([1, 0, 0, 0] * 3)), "VEC4", 3)
    col = add(struct.pack("<12f", *([1, 1, 1, 1] * 3)), "VEC4", 3)
    while len(blob) % 4:
        blob.append(0)
    views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": 6})
    blob.extend(struct.pack("<3H", 0, 1, 2))
    accs.append({"bufferView": len(views) - 1, "componentType": 5123, "count": 3, "type": "SCALAR"})
    idx = len(accs) - 1

    animations = []
    for name, frames in anims:
        times = struct.pack("<%df" % len(frames), *[i / FPS for i in range(len(frames))])
        tacc = add(times, "SCALAR", len(frames), {"min": [0.0], "max": [(len(frames) - 1) / FPS]})
        samplers, channels = [], []
        for k in range(len(skel)):
            parts = [m[k].decompose() for m in frames]
            for target, kind, vals in (
                    ("translation", "VEC3", [list(p[0]) for p in parts]),
                    ("rotation", "VEC4", fix_signs([[p[1].x, p[1].y, p[1].z, p[1].w] for p in parts])),
                    ("scale", "VEC3", [list(p[2]) for p in parts])):
                flat = [x for v in vals for x in v]
                samplers.append({"input": tacc, "interpolation": "LINEAR",
                                 "output": add(struct.pack("<%df" % len(flat), *flat), kind, len(vals))})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": k + 1, "path": target}})
        animations.append({"name": name, "samplers": samplers, "channels": channels})

    gj = {
        "asset": {"version": "2.0", "generator": "jak-enhanced nap_anims.py"},
        "scene": 0, "scenes": [{"nodes": [0, 1]}],
        "nodes": nodes,
        "meshes": [{"name": "jak-nap", "primitives": [{"attributes": {
            "POSITION": pos, "NORMAL": nrm, "TEXCOORD_0": uv, "COLOR_0": col, "JOINTS_0": jnt,
            "WEIGHTS_0": wgt}, "indices": idx, "material": 0}]}],
        "materials": [{"name": "jak-nap"}],
        "skins": [{"inverseBindMatrices": ibm_acc, "joints": list(range(1, len(skel) + 1)), "skeleton": 1}],
        "animations": animations,
        "accessors": accs, "bufferViews": views, "buffers": [{"byteLength": len(blob)}],
    }
    js = json.dumps(gj, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    while len(blob) % 4:
        blob.append(0)
    total = 12 + 8 + len(js) + 8 + len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


def fix_signs(qs):
    for i in range(1, len(qs)):
        if sum(a * b for a, b in zip(qs[i], qs[i - 1])) < 0:
            qs[i] = [-x for x in qs[i]]
    return qs


def breathing(skel, pose, n=96):
    """The sleep loop: one held pose with a slow, big breath (chest swelling, head nodding)"""
    idx = {jn["name"]: k for k, jn in enumerate(skel)}
    out = []
    for i in range(n):
        ph = math.sin(2 * math.pi * i / n)
        fr = list(pose)
        for name, ang, grow in (("upper_body", 0.035, 0.03), ("chest", 0.05, 0.06), ("neckA", -0.06, 0.0)):
            k = idx[name]
            t, r, s = fr[k].decompose()
            r = r @ Quaternion(Vector((1, 0, 0)), ang * ph)
            s = s * (1.0 + grow * max(0.0, ph) + grow * 0.3 * min(0.0, ph))
            fr[k] = Matrix.LocRotScale(t, r, s)
        out.append(fr)
    return out


def stretch_tweak(name, rot, fi, n):
    """The yawn's arms reach further: the rotation from rest pushed 25% on"""
    if name[1:] in ("arm", "forarm", "should"):
        axis, ang = rot.to_axis_angle()
        return Quaternion(axis, ang * 1.25)
    return rot


def main():
    args = sys.argv[sys.argv.index("--") + 1:]
    glb, mixdir, out = args[0], Path(args[1]), args[2]
    skel = jak_skeleton(glb)

    rest, frames = sample_mixamo(mixdir / "getting_up_back.fbx")
    getup_full = retarget(skel, rest, frames)
    m = [k for k, jn in enumerate(skel) if jn["name"] == "main"][0]
    # standing at the end of getting up sits on Jak's origin: lying down starts there, ends where
    # getting up starts
    first, end = getup_full[0][m].translation, getup_full[-1][m].translation
    getup = shift(resample(getup_full, 1.6), skel, (first.x - end.x, first.z - end.z))
    lie = resample(getup, 1.1, reverse=True)
    lying_at = getup[0][m].translation

    rest, frames = sample_mixamo(mixdir / "pose_hands_behind_head_crossed.fbx")
    pose = shift(retarget(skel, rest, frames[:1]), skel, (lying_at.x, lying_at.z))[0]
    sleep = breathing(skel, pose)
    sleep.append(sleep[0])  # loops

    rest, frames = sample_mixamo(mixdir / "yawn.fbx")
    stretch = retarget(skel, rest, frames, stretch_tweak)
    stretch = shift(resample(stretch, 1.3), skel, (0, 0))

    prefix = Path(glb).name.split("-lod0")[0]
    write_glb(out, glb, skel, [(prefix + "-nap-lie", lie), (prefix + "-nap-sleep", sleep),
                                (prefix + "-nap-getup", getup), (prefix + "-nap-stretch", stretch)])
    print("NAP ANIMS", out, "stand %.3f" % STAND_HEIGHT, len(lie), len(sleep), len(getup), len(stretch))


main()
