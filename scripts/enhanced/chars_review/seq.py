"""Drive the characters' review watcher (watch.gc): send a named pad sequence to a running test game
through its dev goalc daemon, wait for "[ca] seq end", print the sequence's [ca] lines.

    seq.py [--port 8205] [--load] <gk log> <name> [name...]      (names: see SEQS)

--port: the dev goalc daemon's control port (8185 + 10 * instance). --load: send watch.gc first.
Steps are (frames to wait before the step, op, arg); ops: 0 tag, 1 press, 2 hold, 3 lefty, 4 leftx,
5 rightx, 6 freeze, 7 end, 8 hook, 9 camera dot x100."""
import json, socket, sys, time

PORT = 8205  # instance 2
X, SQ, CI, TR, R1, L1, UP, RIGHT, DOWN, LEFT, L2, R2 = 0x4000, 0x8000, 0x2000, 0x1000, 0x800, 0x400, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200
TAG, PRESS, HOLD, LY, LX, RX, FREEZE, END, HOOK, CAM = range(10)

SEQS = {
    # on foot, unarmed: idle, walk, run, jumps, attacks, duck/roll
    "foot": [
        (0, HOOK, 1), (60, TAG, 1), (180, TAG, 2), (0, LY, 90), (180, TAG, 3), (0, LY, 0),
        (240, TAG, 4), (0, PRESS, X), (90, TAG, 5), (0, LY, -1),
        (120, TAG, 6), (0, PRESS, X), (90, TAG, 7), (0, PRESS, X), (0, HOLD, X), (20, HOLD, 0),
        (100, TAG, 8), (0, PRESS, X), (0, HOLD, X), (14, HOLD, 0), (2, PRESS, X), (0, HOLD, X), (12, HOLD, 0),
        (110, TAG, 9), (0, PRESS, SQ),
        (90, TAG, 10), (0, PRESS, SQ), (15, PRESS, SQ), (15, PRESS, SQ),
        (130, TAG, 11), (0, PRESS, CI),
        (90, TAG, 12), (0, LY, 0), (60, PRESS, SQ), (60, LY, -1),
        (60, TAG, 13), (0, PRESS, X), (0, HOLD, X), (14, HOLD, 0), (0, PRESS, SQ),
        (140, TAG, 14), (0, PRESS, X), (0, HOLD, X), (14, HOLD, 0), (0, PRESS, CI),
        (120, TAG, 15), (0, HOLD, L1), (30, PRESS, SQ), (25, HOLD, 0),
        (120, TAG, 16), (0, HOLD, L1), (90, LY, 90), (90, LY, -1), (0, HOLD, 0),
        (60, TAG, 17), (0, LY, 0), (60, PRESS, L1), (60, LY, -1),
        (60, TAG, 18), (0, HOLD, L1), (20, PRESS, X), (0, HOLD, L1 | X), (15, HOLD, 0),
        (120, TAG, 19), (60, END, 0),
    ],
    "idle": [(0, TAG, 1), (240, END, 0)],
    "hit": [(0, TAG, 30), (30, HOOK, 2), (150, TAG, 31), (60, END, 0)],
    "death": [(0, TAG, 32), (30, HOOK, 3), (420, TAG, 33), (60, END, 0)],
}


def send(req, timeout=120):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
    s.settimeout(timeout)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        c = s.recv(65536)
        if not c:
            break
        buf += c
    return json.loads(buf.decode("utf-8", "replace"))


def run(log, name, steps=None):
    steps = steps or SEQS[name]
    t = 0
    forms = ["(ca-reset)"]
    for dt, op, arg in steps:
        t += dt
        forms.append("(ca-q %d %d %d)" % (t, op, arg))
    forms.append("(ca-go)")
    size0 = 0
    with open(log, "rb") as f:
        f.seek(0, 2)
        size0 = f.tell()
    r = send({"op": "eval", "form": "(begin " + " ".join(forms) + ")", "timeout": 30})
    if not r.get("ok"):
        print("eval failed", r.get("out", "")[-800:])
        return
    end = time.time() + t / 60.0 * 1.6 + 30
    out = b""
    while time.time() < end:
        time.sleep(1.0)
        with open(log, "rb") as f:
            f.seek(size0)
            out = f.read()
        if b"[ca] seq end" in out:
            break
    print("== %s (%d frames)" % (name, t))
    for ln in out.decode("utf-8", "replace").splitlines():
        if ln.startswith("[ca]"):
            print(ln)


def load():
    import os
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "watch.gc"), encoding="utf-8").read()
    lines = []
    for ln in src.splitlines():
        i = ln.find(";;")
        lines.append(ln if i < 0 else ln[:i])
    r = send({"op": "eval", "form": " ".join(lines), "timeout": 60})
    print("watch.gc", "loaded" if r.get("ok") and "Compilation Error" not in r.get("out", "") else "FAILED: " + r.get("out", "")[-800:])


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--port" in args:
        i = args.index("--port")
        PORT = int(args[i + 1])
        del args[i:i + 2]
    if "--load" in args:
        args.remove("--load")
        load()
    for n in args[1:]:
        run(args[0], n)
