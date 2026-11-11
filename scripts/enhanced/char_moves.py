"""Attack moves of the playable characters (Jak 3 Enhanced: Ashelin, Sig), from generated motion clips.

    python char_moves.py [--clips <dir>] [--out <enhanced-move-tables.gc>] [--sheets <dir>] [--only name,...]

Source clips: Higgsfield Kimodo motion (.npz, original generated motion, 27 joints "cskel27", Y up, 20 fps),
in <main tree>/local/higgsfield/motion (not in git). The clips are slow, stand still for most of their length
and often end before the strike is over, so a move here is a short list of key poses on a stylized
timeline (a snappy anticipation, the strike with a little overshoot, an elastic recovery): each key pose is a
clip frame, usually edited (a limb aimed, the chest turned, a hand put somewhere by two-bone IK), or a pose
built from a clip's stance.

Output: goal_src/jak3/engine/target/enhanced-move-tables.gc, one float table per move, 30 samples a second.
Per sample, vectors in the character's model space (+x left, +y up, +z forward), independent of any rig:
orientation of the pelvis, chest and head (up, forward), direction of the neck, of each arm's and leg's
bones, of each hand (and its thumb side), the pelvis's sway from the root and the lowest ankle's height (in
hip heights), and the long gun's barrel. enhanced-moves.gc aims the character's bones along them.
--sheets draws stick-figure contact sheets of the result (side and front views) to check a move by eye.
"""
import argparse
import math
import os
import sys

import numpy as np

NAMES = ["Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Head", "RightShoulder", "RightArm", "RightForeArm",
         "RightHand", "RightHandEnd", "RightHandThumb1", "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
         "LeftHandEnd", "LeftHandThumb1", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase", "LeftUpLeg",
         "LeftLeg", "LeftFoot", "LeftToeBase"]
J = {n: i for i, n in enumerate(NAMES)}
PARENT = [-1, 0, 1, 2, 3, 4, 5, 4, 7, 8, 9, 10, 10, 4, 13, 14, 15, 16, 16, 0, 19, 20, 21, 0, 23, 24, 25]
# the skeleton's bind pose (T-pose, metres): joint positions
BIND = np.array([
    [0.000, 0.970, 0.000], [0.000, 1.041, -0.047], [0.000, 1.134, -0.064], [0.000, 1.228, -0.072],
    [0.000, 1.323, -0.072], [0.000, 1.571, -0.037], [0.000, 1.699, -0.014], [-0.032, 1.496, -0.019],
    [-0.191, 1.496, -0.019], [-0.486, 1.496, -0.019], [-0.719, 1.496, -0.019], [-0.789, 1.496, -0.019],
    [-0.752, 1.496, 0.013], [0.032, 1.496, -0.019], [0.191, 1.496, -0.019], [0.486, 1.496, -0.019],
    [0.719, 1.496, -0.019], [0.789, 1.496, -0.019], [0.752, 1.496, 0.013], [-0.095, 0.942, 0.000],
    [-0.095, 0.530, 0.000], [-0.095, 0.074, 0.000], [-0.095, 0.015, 0.161], [0.095, 0.942, 0.000],
    [0.095, 0.530, 0.000], [0.095, 0.074, 0.000], [0.095, 0.015, 0.161]])
HIP_H = 0.97
ANKLE_H = 0.074
RATE = 30.0
X, Y, Z = np.eye(3)

# a table sample's vectors, in order (enhanced-moves.gc reads them by index)
DIRS = [("Neck", "Head"),
        ("LeftArm", "LeftForeArm"), ("LeftForeArm", "LeftHand"), ("LeftHand", "LeftHandEnd"), ("LeftHand", "LeftHandThumb1"),
        ("RightArm", "RightForeArm"), ("RightForeArm", "RightHand"), ("RightHand", "RightHandEnd"), ("RightHand", "RightHandThumb1"),
        ("LeftUpLeg", "LeftLeg"), ("LeftLeg", "LeftFoot"), ("LeftFoot", "LeftToeBase"),
        ("RightUpLeg", "RightLeg"), ("RightLeg", "RightFoot"), ("RightFoot", "RightToeBase")]
SLOTS = 6 + len(DIRS) + 2   # frames (6), directions, sway/clearance, gun barrel


# ---- rotations ---------------------------------------------------------------------------------

def unit(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-9 else v


def rot_axis(axis, ang):
    a = unit(np.asarray(axis, float))
    K = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return np.eye(3) + math.sin(ang) * K + (1 - math.cos(ang)) * (K @ K)


def rot_between(a, b):
    a, b = unit(a), unit(b)
    ax = np.cross(a, b)
    s, c = np.linalg.norm(ax), float(np.dot(a, b))
    if s < 1e-8:
        if c > 0:
            return np.eye(3)
        ax = np.cross(a, X if abs(a[0]) < 0.9 else Y)
        return rot_axis(ax, math.pi)
    return rot_axis(ax, math.atan2(s, c))


def log_rot(R):
    c = max(-1.0, min(1.0, (np.trace(R) - 1) / 2))
    ang = math.acos(c)
    if ang < 1e-7:
        return np.zeros(3)
    if math.pi - ang < 1e-4:
        w, v = np.linalg.eigh((R + np.eye(3)) / 2)
        return unit(v[:, 2]) * ang
    return np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]]) * (ang / (2 * math.sin(ang)))


def exp_rot(v):
    ang = np.linalg.norm(v)
    return np.eye(3) if ang < 1e-9 else rot_axis(v, ang)


def interp_rot(R0, R1, u):
    return exp_rot(u * log_rot(R1 @ R0.T)) @ R0


def interp_dir(d0, d1, u):
    ax = np.cross(d0, d1)
    s, c = np.linalg.norm(ax), float(np.dot(d0, d1))
    if s < 1e-8:
        return d0.copy()
    return rot_axis(ax, u * math.atan2(s, c)) @ d0


def deg(a):
    return math.radians(a)


# ---- a pose --------------------------------------------------------------------------------------

class Rig:
    """Joint positions (27 x 3, metres, the root on the ground at x = z = 0) and the pelvis's, chest's and
    head's turn from the bind pose."""

    def __init__(self, P, pelvis, chest, head):
        self.P = np.array(P, float)
        self.frames = {"Hips": np.array(pelvis, float), "Spine3": np.array(chest, float), "Head": np.array(head, float)}
        self.gun = None   # the long gun's barrel direction, when the move sets it

    def copy(self):
        r = Rig(self.P, self.frames["Hips"], self.frames["Spine3"], self.frames["Head"])
        r.gun = None if self.gun is None else self.gun.copy()
        return r

    @staticmethod
    def subtree(j):
        out = [j]
        for k in range(len(PARENT)):
            p = PARENT[k]
            while p >= 0 and p != j:
                p = PARENT[p]
            if p == j and k != j:
                out.append(k)
        return out

    def rot(self, joint, R, pivot=None):
        """Turn a joint and all that hangs from it about itself (or pivot)."""
        j = J[joint]
        pv = self.P[j].copy() if pivot is None else np.asarray(pivot, float)
        sub = self.subtree(j)
        self.P[sub] = (self.P[sub] - pv) @ R.T + pv
        for name in self.frames:
            if J[name] in sub:
                self.frames[name] = R @ self.frames[name]
        return self

    def turn(self, joint, axis, degrees):
        return self.rot(joint, rot_axis(axis, deg(degrees)))

    def dir(self, a, b):
        return unit(self.P[J[b]] - self.P[J[a]])

    def aim(self, joint, child, d):
        return self.rot(joint, rot_between(self.dir(joint, child), unit(np.asarray(d, float))))

    def ik(self, a, b, c, target, pole=None):
        """Two-bone IK: joints a-b-c with c put at target (as far as the bones reach), b bending toward pole
        (a direction; default: where it bends now). c's own turn and what hangs from it stay as they are."""
        A = self.P[J[a]].copy()
        l1 = np.linalg.norm(self.P[J[b]] - A)
        l2 = np.linalg.norm(self.P[J[c]] - self.P[J[b]])
        t = np.asarray(target, float) - A
        d = min(np.linalg.norm(t), 0.999 * (l1 + l2))
        d = max(d, abs(l1 - l2) + 1e-3)
        n = unit(t)
        hint = (self.P[J[b]] - A) if pole is None else np.asarray(pole, float)
        hint = hint - n * np.dot(hint, n)
        hint = unit(hint) if np.linalg.norm(hint) > 1e-6 else unit(np.cross(n, X))
        along = (l1 * l1 - l2 * l2 + d * d) / (2 * d)
        h = math.sqrt(max(0.0, l1 * l1 - along * along))
        elbow = A + n * along + hint * h
        end = A + n * d
        sub_c = self.subtree(J[c])
        keep = self.P[sub_c] - self.P[J[c]]
        frames = {k: v.copy() for k, v in self.frames.items()}
        self.rot(a, rot_between(self.P[J[b]] - A, elbow - A))
        self.rot(b, rot_between(self.P[J[c]] - self.P[J[b]], end - self.P[J[b]]))
        self.P[sub_c] = keep + self.P[J[c]]
        self.frames = frames
        return self

    def move(self, d):
        self.P += np.asarray(d, float)
        return self

    def yaw(self, degrees):
        R = rot_axis(Y, deg(degrees))
        self.P = self.P @ R.T
        for k in self.frames:
            self.frames[k] = R @ self.frames[k]
        if self.gun is not None:
            self.gun = R @ self.gun
        return self

    def ground(self, clearance=0.0):
        """Up or down so the lowest ankle is at its standing height (plus clearance)."""
        low = min(self.P[J["LeftFoot"], 1], self.P[J["RightFoot"], 1])
        self.P[:, 1] += ANKLE_H + clearance - low
        return self

    def mirror(self):
        """Left and right swapped."""
        P = self.P.copy()
        P[:, 0] *= -1
        Q = P.copy()
        for n in NAMES:
            if n.startswith("Left"):
                o = "Right" + n[4:]
                Q[J[n]], Q[J[o]] = P[J[o]], P[J[n]]
        M = np.diag([-1.0, 1.0, 1.0])
        r = Rig(Q, M @ self.frames["Hips"] @ M, M @ self.frames["Spine3"] @ M, M @ self.frames["Head"] @ M)
        if self.gun is not None:
            r.gun = M @ self.gun
        return r


class Feat:
    """What a table sample holds: three turns, the bone directions, the pelvis's place, the gun's barrel."""

    def __init__(self, rig):
        self.pelvis = rig.frames["Hips"].copy()
        self.chest = rig.frames["Spine3"].copy()
        self.head = rig.frames["Head"].copy()
        self.dirs = np.array([rig.dir(a, b) for a, b in DIRS])
        self.pos = rig.P[0].copy()
        self.gun = None if rig.gun is None else unit(rig.gun)

    @staticmethod
    def mix(a, b, u):
        f = Feat.__new__(Feat)
        f.pelvis = interp_rot(a.pelvis, b.pelvis, u)
        f.chest = interp_rot(a.chest, b.chest, u)
        f.head = interp_rot(a.head, b.head, u)
        f.dirs = np.array([interp_dir(a.dirs[i], b.dirs[i], u) for i in range(len(DIRS))])
        f.pos = a.pos + (b.pos - a.pos) * u
        ga = a.gun if a.gun is not None else b.gun
        gb = b.gun if b.gun is not None else a.gun
        f.gun = None if ga is None else interp_dir(ga, gb, u)
        return f

    def joints(self):
        """Joint positions again (for the sheets and the ankles' height): the bind pose's bone lengths."""
        P = np.zeros((27, 3))
        P[0] = self.pos
        half = interp_rot(self.pelvis, self.chest, 0.5)
        for j in (1, 2, 3, 4):
            P[j] = P[0] + half @ (BIND[j] - BIND[0])
        for n in ("Neck", "RightShoulder", "RightArm", "LeftShoulder", "LeftArm"):
            P[J[n]] = P[4] + self.chest @ (BIND[J[n]] - BIND[4])
        for n in ("RightUpLeg", "LeftUpLeg"):
            P[J[n]] = P[0] + self.pelvis @ (BIND[J[n]] - BIND[0])
        for i, (a, b) in enumerate(DIRS):
            P[J[b]] = P[J[a]] + self.dirs[i] * np.linalg.norm(BIND[J[b]] - BIND[J[a]])
        return P

    def clearance(self):
        P = self.joints()
        return max(0.0, min(P[J["LeftFoot"], 1], P[J["RightFoot"], 1]) - ANKLE_H)

    def slots(self):
        out = [self.pelvis @ Y, self.pelvis @ Z, self.chest @ Y, self.chest @ Z, self.head @ Y, self.head @ Z]
        out.extend(self.dirs)
        out.append(np.array([self.pos[0] / HIP_H, self.clearance() / HIP_H, self.pos[2] / HIP_H]))
        out.append(self.gun if self.gun is not None else np.zeros(3))
        return np.array(out)


# ---- the clips -----------------------------------------------------------------------------------

class Clip:
    def __init__(self, path):
        d = np.load(path, allow_pickle=False)
        self.P = d["posed_joints"].astype(float)
        self.G = d["global_rot_mats"].astype(float)
        self.smooth = d["smooth_root_pos"].astype(float)
        self.n = self.P.shape[0]

    def rig(self, f, face=None):
        """Frame f as a pose: the root (the clip's smoothed path) at x = z = 0, turned so that `face` (a joint
        pair, e.g. the kicking leg hip to foot, or a number of degrees) looks along +z. Default: the pelvis."""
        P = self.P[f] - np.array([self.smooth[f, 0], 0.0, self.smooth[f, 2]])
        G = self.G[f]
        if face is None:
            d = G[0] @ Z
        elif isinstance(face, (int, float)):
            d = rot_axis(Y, deg(face)) @ Z
        else:
            d = self.P[f, J[face[1]]] - self.P[f, J[face[0]]]
        R = rot_axis(Y, -math.atan2(d[0], d[2]))
        return Rig(P @ R.T, R @ G[0], R @ G[J["Spine3"]], R @ G[J["Head"]])


_clips = {}
CLIP_DIR = ""


def clip(name):
    if name not in _clips:
        _clips[name] = Clip(os.path.join(CLIP_DIR, name + ".npz"))
    return _clips[name]


# ---- timing --------------------------------------------------------------------------------------

def ease(kind, u):
    u = max(0.0, min(1.0, u))
    if kind == "in":      # slow start, arrives fast (a strike)
        return u ** 2.4
    if kind == "snap":    # arrives very fast
        return u ** 3.5
    if kind == "out":     # leaves fast, settles
        return 1 - (1 - u) ** 2.4
    if kind == "out3":
        return 1 - (1 - u) ** 3.5
    if kind == "io":
        return u * u * (3 - 2 * u)
    return u


def spring(u):
    """After a strike: past the pose and back, dying out (u 0-1 over the hold)."""
    return math.exp(-4.5 * u) * math.sin(2 * math.pi * 1.15 * u)


class Move:
    """Key poses on a timeline. keys: (seconds, pose, ease into it, overshoot). A key with overshoot is held
    until the next key's time minus that key's own travel: give the hold as a key with the same pose (pose
    None = the previous key's), during which the pose springs past it along the way it arrived."""

    def __init__(self, name, who, keys, note="", gun=False):
        self.name, self.who, self.note, self.gun = name, who, note, gun
        self.keys = []
        last = None
        for t, pose, kind, over in keys:
            if pose is None:
                feat, hold = last, True
            else:
                feat, hold = Feat(pose), False
            self.keys.append((t, feat, kind, over, hold))
            last = feat
        self.length = self.keys[-1][0]

    def at(self, t):
        ks = self.keys
        if t <= ks[0][0]:
            return ks[0][1]
        for i in range(1, len(ks)):
            t0, f0 = ks[i - 1][0], ks[i - 1][1]
            t1, f1, kind, over, hold = ks[i]
            if t <= t1 or i == len(ks) - 1:
                u = (t - t0) / max(1e-6, t1 - t0)
                if hold:
                    # the pose arrived at ks[i-1] from ks[i-2]: spring along that way
                    o = ks[i - 1][3]
                    if o and i >= 2:
                        return Feat.mix(ks[i - 2][1], f0, 1.0 + o * spring(min(1.0, u)))
                    return f0
                return Feat.mix(f0, f1, ease(kind, u))
        return ks[-1][1]

    def samples(self):
        n = int(round(self.length * RATE)) + 1
        return [self.at(i / RATE) for i in range(n)]


# ---- the moves -----------------------------------------------------------------------------------

def plant(rig, d=(0.0, 0.0, 0.0), lfoot=(0.0, 0.0, 0.0), rfoot=(0.0, 0.0, 0.0)):
    """The body moved by d with the feet staying where they are (or moved by lfoot / rfoot): the knees bend."""
    L = rig.P[J["LeftFoot"]] + np.asarray(lfoot, float)
    R = rig.P[J["RightFoot"]] + np.asarray(rfoot, float)
    rig.move(d)
    rig.ik("LeftUpLeg", "LeftLeg", "LeftFoot", L, pole=rig.frames["Hips"] @ np.array([0.2, 0.0, 1.0]))
    rig.ik("RightUpLeg", "RightLeg", "RightFoot", R, pole=rig.frames["Hips"] @ np.array([-0.2, 0.0, 1.0]))
    return rig


def twist(rig, degrees):
    """The chest (arms and head with it) about the vertical: + to the left."""
    return rig.turn("Spine1", Y, degrees)


def lean(rig, degrees):
    """The chest forward (+) or back, about the left-right line of the hips."""
    return rig.turn("Spine1", rig.frames["Hips"] @ X, degrees)


def look(rig, yaw=0.0):
    """The head facing yaw degrees left of the model's forward."""
    f = rig.frames["Head"] @ Z
    return rig.turn("Neck", Y, yaw - math.degrees(math.atan2(f[0], f[2])))


def roll(rig, joint, child, ref, to):
    """A bone turned about its own line until its side (toward joint ref) looks along to."""
    axis = rig.dir(joint, child)
    cur = rig.P[J[ref]] - rig.P[J[joint]]
    cur = cur - axis * np.dot(cur, axis)
    t = np.asarray(to, float)
    t = t - axis * np.dot(t, axis)
    if np.linalg.norm(cur) < 1e-6 or np.linalg.norm(t) < 1e-6:
        return rig
    cur, t = unit(cur), unit(t)
    return rig.rot(joint, rot_axis(axis, math.atan2(float(np.dot(np.cross(cur, t), axis)), float(np.dot(cur, t)))))


def arm(rig, side, upper, fore, hand=None, thumb=None):
    """An arm by directions (model space): upper arm, forearm, the line of the hand (default: the forearm's)
    and the way its thumb side looks."""
    rig.aim(side + "Arm", side + "ForeArm", upper)
    rig.aim(side + "ForeArm", side + "Hand", fore)
    rig.aim(side + "Hand", side + "HandEnd", fore if hand is None else hand)
    if thumb is not None:
        roll(rig, side + "Hand", side + "HandEnd", side + "HandThumb1", thumb)
    return rig


def reach(rig, side, target, pole, hand=None, thumb=None):
    """A hand put at target (two-bone IK, the elbow toward pole)."""
    rig.ik(side + "Arm", side + "ForeArm", side + "Hand", target, pole=pole)
    rig.aim(side + "Hand", side + "HandEnd", rig.dir(side + "ForeArm", side + "Hand") if hand is None else hand)
    if thumb is not None:
        roll(rig, side + "Hand", side + "HandEnd", side + "HandThumb1", thumb)
    return rig


def ashelin_front_kick():
    """Karate front snap kick (Ashelin, square). From ashelin_front_kick_3: the only take that chambers the knee
    and snaps the shin out at chest height with the leg straight (foot 1.29 m up, knee 169 degrees; take 1 is
    cleaner but only hip high, take 2 never straightens: 129). Frames 22-31, faced along the kick."""
    k = clip("ashelin_front_kick_3")
    d = k.P[26, J["RightFoot"]] - k.P[26, J["Hips"]]
    yaw = math.degrees(math.atan2(d[0], d[2]))
    # from her stance (the guard of take 1), the weight going onto the left leg
    ready = clip("ashelin_front_kick_1").rig(4)
    chamber = k.rig(22, yaw)
    # a tighter, higher chamber: the knee up to the chest, the heel under the thigh, leaning back a little
    chamber.aim("RightUpLeg", "RightLeg", [-0.10, 0.45, 0.89])
    chamber.aim("RightLeg", "RightFoot", [0.0, -0.96, -0.28])
    chamber.turn("Spine1", X, -8)
    strike = k.rig(26, yaw)
    # the leg straight along the kick, toes pulled back (ball of the foot), hips pushed in
    leg = unit(strike.P[J["RightFoot"]] - strike.P[J["RightUpLeg"]])
    leg = unit(np.array([0.0, leg[1] - 0.04, math.hypot(leg[0], leg[2])]))
    strike.aim("RightUpLeg", "RightLeg", leg)
    strike.aim("RightLeg", "RightFoot", leg)
    strike.aim("RightFoot", "RightToeBase", unit(leg + np.array([0.0, 0.9, 0.0])))
    strike.turn("Spine1", X, -6)
    strike.move([0.0, 0.0, 0.05])
    back = k.rig(28, yaw)
    back.aim("RightUpLeg", "RightLeg", [-0.08, 0.25, 0.96])
    back.aim("RightLeg", "RightFoot", [0.0, -0.97, -0.24])
    down = k.rig(31, yaw)
    end = clip("ashelin_front_kick_1").rig(4)
    return Move("ashelin-front-kick", "ashelin", [
        (0.00, ready, "lin", 0),
        (0.10, chamber, "out", 0),
        (0.19, strike, "snap", 0.10),
        (0.27, None, "lin", 0),
        (0.36, back, "out", 0),
        (0.47, down, "io", 0),
        (0.64, end, "io", 0),
    ], note="ashelin_front_kick_3 frames 22-31 (chamber and strike sharpened), from and to the stance of take 1")


def ashelin_pistol_whip():
    """The grip of her pistol brought down from over her head (square with the pistol out). No take holds the
    strike (1 and 2 stand aiming for 1.4 s; 3 raises the arm and is cut as it comes down, the hand never under
    1.3 m): the stance is the guard of ashelin_front_kick_1, the step in is the kind take 3 makes, the arm is
    posed here."""
    g = clip("ashelin_front_kick_1").rig(4)
    a = g.copy()
    plant(a, (0.0, -0.03, -0.05), rfoot=(0.0, 0.05, 0.08))
    twist(a, -28)
    lean(a, -10)
    arm(a, "Right", (-0.35, 0.82, -0.20), (0.20, 0.80, -0.56), hand=(0.20, 0.70, -0.68), thumb=(0.0, -0.4, -0.9))
    arm(a, "Left", (0.30, 0.02, 0.95), (0.05, 0.22, 0.97), thumb=(-0.8, 0.5, 0.0))
    look(a)
    s = g.copy()
    plant(s, (0.0, -0.10, 0.22), rfoot=(0.0, 0.0, 0.36))
    twist(s, 24)
    lean(s, 23)
    arm(s, "Right", (-0.08, -0.52, 0.85), (0.05, -0.68, 0.73), hand=(0.05, -0.55, 0.83), thumb=(0.0, 0.85, 0.52))
    arm(s, "Left", (0.45, -0.55, -0.70), (0.0, 0.25, 0.97), thumb=(-0.5, 0.8, 0.0))
    look(s)
    r = g.copy()
    plant(r, (0.0, -0.03, 0.10), rfoot=(0.0, 0.0, 0.18))
    lean(r, 8)
    return Move("ashelin-pistol-whip", "ashelin", [
        (0.00, g, "lin", 0),
        (0.10, a, "out", 0),
        (0.19, s, "snap", 0.09),
        (0.28, None, "lin", 0),
        (0.44, r, "out", 0),
        (0.64, g, "io", 0),
    ], note="posed on the stance of ashelin_front_kick_1 (no pistol-whip take holds the strike)")


def sig_stance():
    return clip("sig_boxer_punch_1").rig(10)


def sig_elbow():
    """A forward elbow strike (Sig, square). The elbow takes never bring the elbow in front (0.2 m ahead of
    its shoulder at most, the forearm open at 80-114 degrees), so the stance is the guard of a take and the
    coil, the step and the folded arm are posed here."""
    g = sig_stance()
    a = g.copy()
    plant(a, (0.0, -0.07, -0.07))
    twist(a, -34)
    arm(a, "Right", (-0.52, -0.30, -0.80), (0.25, 0.62, 0.74), thumb=(0.4, 0.9, 0.0))
    arm(a, "Left", (0.25, -0.45, 0.86), (-0.25, 0.80, 0.55), thumb=(-0.6, 0.6, 0.5))
    look(a)
    s = g.copy()
    plant(s, (0.0, -0.06, 0.20), lfoot=(0.0, 0.0, 0.26))
    twist(s, 44)
    lean(s, 16)
    arm(s, "Right", (0.14, 0.24, 0.96), (0.66, 0.26, -0.70), thumb=(0.2, 0.95, 0.25))
    arm(s, "Left", (0.42, -0.62, -0.66), (-0.15, 0.40, 0.90), thumb=(-0.5, 0.8, 0.0))
    look(s)
    r = g.copy()
    plant(r, (0.0, -0.02, 0.08), lfoot=(0.0, 0.0, 0.13))
    twist(r, -9)
    return Move("sig-elbow", "sig", [
        (0.00, g, "lin", 0),
        (0.09, a, "out", 0),
        (0.18, s, "snap", 0.08),
        (0.27, None, "lin", 0),
        (0.43, r, "out", 0),
        (0.64, g, "io", 0),
    ], note="posed on the guard of sig_boxer_punch_1 (the elbow takes never bring the elbow in front)")


def sig_boxer():
    """A jab and a straight (the other choice for the square of Sig). Take 1 only throws a late right overhand
    and take 2 a small jab and a left hook: the two punches are posed on the guard of take 1."""
    g = sig_stance()
    jab = g.copy()
    plant(jab, (0.0, -0.02, 0.07))
    twist(jab, -24)
    arm(jab, "Left", (0.06, 0.04, 1.0), (0.0, 0.05, 1.0), thumb=(-0.9, 0.4, 0.0))
    look(jab)
    mid = g.copy()
    plant(mid, (0.0, -0.03, 0.08))
    twist(mid, 6)
    st = g.copy()
    plant(st, (0.0, -0.05, 0.17), lfoot=(0.0, 0.0, 0.12))
    twist(st, 40)
    lean(st, 9)
    arm(st, "Right", (-0.06, 0.04, 1.0), (0.0, 0.06, 1.0), thumb=(0.9, 0.4, 0.0))
    look(st)
    r = g.copy()
    plant(r, (0.0, -0.02, 0.06), lfoot=(0.0, 0.0, 0.06))
    twist(r, -8)
    return Move("sig-boxer", "sig", [
        (0.00, g, "lin", 0),
        (0.08, jab, "in", 0),
        (0.15, mid, "out", 0),
        (0.22, st, "snap", 0.07),
        (0.30, None, "lin", 0),
        (0.45, r, "out", 0),
        (0.64, g, "io", 0),
    ], note="posed on the guard of sig_boxer_punch_1 (take 1: one late overhand; take 2: a small jab and a hook)")


def sig_spin_punch():
    """A spinning backfist (Sig, circle): a full turn to his left on his left foot, the left arm whipping out
    as he comes round. Only take 2 turns at all (280 degrees, arms tucked, cut before the fist goes out), takes
    1 and 3 swing the arms standing: the turn and the arm are posed on the guard of a take."""
    g = sig_stance()
    low = g.copy()
    plant(low, (0.0, -0.07, 0.0))
    # (about a point a third of the way to his left foot: on the foot itself his body swung 0.4 m out and back)
    pivot = 0.35 * np.array([low.P[J["LeftFoot"], 0], 0.0, low.P[J["LeftFoot"], 2]])

    def pose(psi, ext, lift=0.0, crouch=0.0):
        r = low.copy()
        if crouch:
            plant(r, (0.0, -crouch, 0.0))
        if lift:
            plant(r, rfoot=(0.0, lift, 0.0))
        # the left arm: folded across his chest (0) to straight out to his left at shoulder height (1)
        fold_u, fold_f = np.array([-0.30, -0.50, 0.81]), np.array([-0.80, 0.50, 0.33])
        out_u, out_f = np.array([0.98, 0.10, -0.15]), np.array([1.0, 0.06, 0.05])
        arm(r, "Left", interp_dir(unit(fold_u), unit(out_u), ext), interp_dir(unit(fold_f), unit(out_f), ext),
            thumb=(0.0, 1.0, 0.0))
        twist(r, -30 * ext + 12)
        look(r, 35 if ext > 0.5 else 0)
        r.move(-pivot)
        r.yaw(psi)
        r.move(pivot)
        return r

    return Move("sig-spin-punch", "sig", [
        (0.00, pose(0, 0.0), "lin", 0),
        (0.05, pose(-26, 0.0, crouch=0.05), "out", 0),
        (0.10, pose(55, 0.0, lift=0.05), "in", 0),
        (0.145, pose(145, 0.1, lift=0.07), "lin", 0),
        (0.185, pose(222, 0.6, lift=0.06), "lin", 0),
        (0.22, pose(282, 1.0, lift=0.03), "lin", 0),
        (0.26, pose(328, 1.0), "out", 0),
        (0.32, pose(371, 0.5), "out", 0),
        (0.44, pose(357, 0.1), "io", 0),
        (0.64, g.copy().yaw(360), "io", 0),
    ], note="posed on the guard of sig_boxer_punch_1 (only take 2 turns, 280 degrees, with no fist)")


def sig_rifle_butt():
    """The butt of the Peacemaker thrust forward at head height (Sig, square with it out). The takes hold a
    rifle pose and barely move (the hands go 6 cm forward): the hold is posed here, both hands on the gun, the
    barrel along the line from his right hand (the grip) to his left."""
    g = sig_stance()

    def hold(r, right, left):
        reach(r, "Right", right, pole=(-0.7, -0.6, -0.3))
        reach(r, "Left", left, pole=(0.7, -0.6, 0.0))
        r.gun = unit(r.P[J["LeftHand"]] - r.P[J["RightHand"]])
        r.aim("RightHand", "RightHandEnd", r.gun)
        r.aim("LeftHand", "LeftHandEnd", unit(np.cross(r.gun, Y)) * -1.0)
        look(r)
        return r

    h = hold(g.copy(), (-0.20, 1.10, 0.22), (0.10, 1.24, 0.48))
    a = g.copy()
    plant(a, (0.0, -0.06, -0.08))
    twist(a, -30)
    hold(a, (-0.36, 1.04, -0.08), (0.02, 1.28, 0.34))
    s = g.copy()
    plant(s, (0.0, -0.05, 0.20), lfoot=(0.0, 0.0, 0.24))
    twist(s, 40)
    lean(s, 12)
    hold(s, (-0.02, 1.48, 0.66), (0.28, 1.34, 0.24))
    r = g.copy()
    plant(r, (0.0, -0.02, 0.08), lfoot=(0.0, 0.0, 0.12))
    twist(r, -8)
    hold(r, (-0.22, 1.12, 0.20), (0.10, 1.25, 0.46))
    return Move("sig-rifle-butt", "sig", [
        (0.00, h, "lin", 0),
        (0.09, a, "out", 0),
        (0.18, s, "snap", 0.08),
        (0.27, None, "lin", 0),
        (0.44, r, "out", 0),
        (0.64, h, "io", 0),
    ], note="posed on the guard of sig_boxer_punch_1 (the rifle-butt takes barely move)", gun=True)


MOVES = [ashelin_front_kick, ashelin_pistol_whip, sig_elbow, sig_boxer, sig_spin_punch, sig_rifle_butt]


# ---- output --------------------------------------------------------------------------------------

LIMBS = [("Hips", "Spine3", 0), ("Spine3", "Neck", 0), ("Neck", "Head", 0),
         ("Spine3", "RightArm", 1), ("RightArm", "RightForeArm", 1), ("RightForeArm", "RightHand", 1), ("RightHand", "RightHandEnd", 1),
         ("Spine3", "LeftArm", 2), ("LeftArm", "LeftForeArm", 2), ("LeftForeArm", "LeftHand", 2), ("LeftHand", "LeftHandEnd", 2),
         ("Hips", "RightUpLeg", 1), ("RightUpLeg", "RightLeg", 1), ("RightLeg", "RightFoot", 1), ("RightFoot", "RightToeBase", 1),
         ("Hips", "LeftUpLeg", 2), ("LeftUpLeg", "LeftLeg", 2), ("LeftLeg", "LeftFoot", 2), ("LeftFoot", "LeftToeBase", 2)]
COL = [(40, 40, 40), (210, 40, 40), (40, 90, 210)]   # body, right side red, left side blue


def sheet(move, path, views=("side", "front"), step=2):
    """Stick figures of every step-th sample: a row per view (side: +z to the right; front: seen from ahead;
    top: from above, +z up the page)."""
    from PIL import Image, ImageDraw
    feats = move.samples()[::step]
    w, h = 96, 150
    img = Image.new("RGB", (w * len(feats) + 4, h * len(views) + 14), (245, 245, 240))
    dr = ImageDraw.Draw(img)
    scale = (h - 22) / 2.15
    dr.text((2, 1), "%s  %.2f s, a figure every %.3f s" % (move.name, move.length, step / RATE), fill=(0, 0, 0))
    for r, view in enumerate(views):
        for k, f in enumerate(feats):
            P = f.joints()
            ox, oy = 2 + k * w + w / 2, 14 + r * h + h - 6
            dr.line([(2 + k * w, oy), (2 + (k + 1) * w, oy)], fill=(190, 190, 180))

            def pt(p):
                if view == "side":
                    return (ox + p[2] * scale, oy - p[1] * scale)
                if view == "front":
                    return (ox - p[0] * scale, oy - p[1] * scale)
                return (ox - p[0] * scale, oy - (p[2] + 0.9) * scale)
            for a, b, col in LIMBS:
                dr.line([pt(P[J[a]]), pt(P[J[b]])], fill=COL[col], width=2)
            hx, hy = pt(P[J["Head"]])
            dr.ellipse([hx - 5, hy - 8, hx + 5, hy + 2], outline=(40, 40, 40))
            if f.gun is not None:
                g0 = P[J["RightHand"]]
                dr.line([pt(g0 - f.gun * 0.35), pt(g0 + f.gun * 0.75)], fill=(20, 150, 60), width=2)
            if r == 0:
                dr.text((2 + k * w + 2, 14 + r * h + 1), "%.2f" % (k * step / RATE), fill=(110, 110, 110))
    img.save(path)
    return path


def gc_name(name):
    return "*enhanced-move-table-" + name + "*"


def write_gc(moves, path):
    out = [";;-*-Lisp-*-", "(in-package goal)", "",
           ";; name: enhanced-move-tables.gc", ";; name in dgo: enhanced-move-tables", ";; dgos: GAME", "",
           ";; Enhanced: the playable characters' attack moves (enhanced-moves.gc plays them).",
           ";; MADE BY scripts/enhanced/char_moves.py from generated motion clips (original, not the game's): don't edit.",
           ";; Per sample (%d a second) %d vectors in the character's model space (+x left, +y up, +z forward):" % (int(RATE), SLOTS),
           ";;   0-5   pelvis up, pelvis forward, chest up, chest forward, head up, head forward",
           ";;   6     neck (toward the head)",
           ";;   7-10  left upper arm, forearm, hand, the hand's thumb side;  11-14 the right arm's",
           ";;   15-17 left thigh, shin, foot;  18-20 the right leg's",
           ";;   21    pelvis sway x, lowest ankle's height over standing, sway z (in hip heights)",
           ";;   22    the long gun's barrel (zero: none)",
           "",
           "(defconstant ENHANCED_MOVE_SLOTS %d)" % SLOTS,
           "(defconstant ENHANCED_MOVE_RATE %.1f)" % RATE, ""]
    for m in moves:
        feats = m.samples()
        out.append(";; %s (%s): %.2f s. %s" % (m.name, m.who, m.length, m.note))
        out.append("(define %s (new 'static 'array float %d" % (gc_name(m.name), len(feats) * SLOTS * 3))
        for f in feats:
            s = f.slots()
            out.append("  " + " ".join("%.3f" % (v + 0.0) for v in s.reshape(-1)).replace("-0.000", "0.000"))
        out.append("  ))")
        out.append("(defconstant %s %d)" % ("ENHANCED_MOVE_" + m.name.upper().replace("-", "_") + "_SAMPLES", len(feats)))
        out.append("")
    text = "\r\n".join(out) + "\r\n"
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return path


def main():
    global CLIP_DIR
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser()
    ap.add_argument("--clips", default="")
    ap.add_argument("--out", default=os.path.join(repo, "goal_src", "jak3", "engine", "target", "enhanced-move-tables.gc"))
    ap.add_argument("--sheets", default="")
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    CLIP_DIR = args.clips
    if not CLIP_DIR:
        # the main tree's local folder (a worktree has none of its own)
        import subprocess
        common = subprocess.run(["git", "-C", repo, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True).stdout.strip()
        CLIP_DIR = os.path.join(os.path.dirname(common) if common else repo, "local", "higgsfield", "motion")
    only = [s for s in args.only.split(",") if s]
    moves = [fn() for fn in MOVES]
    for m in moves:
        feats = m.samples()
        cl = [f.clearance() for f in feats]
        print("%-22s %-8s %.2f s, %d samples, lowest ankle 0..%.2f m over standing" % (m.name, m.who, m.length, len(feats), max(cl)))
        if args.sheets and (not only or m.name in only):
            os.makedirs(args.sheets, exist_ok=True)
            views = ("side", "top") if "spin" in m.name else ("side", "front")
            print("  ", sheet(m, os.path.join(args.sheets, m.name + ".png"), views=views, step=1 if "spin" in m.name else 2))
    if args.out != "-":
        print("wrote", write_gc(moves, args.out))


if __name__ == "__main__":
    main()
