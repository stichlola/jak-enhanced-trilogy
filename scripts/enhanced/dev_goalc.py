"""A goalc kept running for one game instance, with a reply channel (Jak Enhanced dev loop).

    python dev_goalc.py --instance 1 --bin <shadow bin> [--mi] [--lt]

goalc's own nREPL socket never answers, and its stdout is block-buffered when it is a pipe. This
daemon owns goalc (stdin kept as an open pipe, so goalc doesn't spin a core on EOF; goalc sits in a
job object, so it dies with the daemon) and serves JSON lines on 127.0.0.1:8185+10*n:

  {"op": "eval", "form": "(+ 1 2)", "timeout": 60}  -> {"ok", "ms", "out", "prompt"}
  {"op": "reload", "files": [...], "all": false, "force": false, "lt": true}
                                                     -> {"ok", "results": [...], "notes": [...]}
  {"op": "mi"}       (mi) in the background (the next boot's CGO/DGO get the changes)
  {"op": "status"}   {"pid", "goalc_pid", "instance", "mi", "connected", "log", "bin"}
  {"op": "stop"}

How an answer is found: each form goes through goalc's nREPL, followed by the marker form
(seval (error "DEVMARK-<k>")). The marker fails on purpose: goalc prints "REPL Error" through its
logger, which flushes stdout, so everything the first form printed is in the pipe by then.
reload = (ml "<file>") per file: compile, write the .o, load it into the connected game.
"""
import argparse
import ctypes
import hashlib
import json
import os
import re
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time

EVAL = 10
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
PROMPT = re.compile(r"(g |gc|gs|gr)>\s*$")
PROMPTS = re.compile(r"(?<![\w-])(g |gc|gs|gr)> ")
GAME = "jak3"


def instance_offset(n):
    return 10 * n if 0 < n < 100 else 0


# (mi)'s progress entries: "[ 42%] [goalc   ] " + 6 chars (blank, or the seconds) + the 70-char
# name field (MakeSystem.cpp); with \r gone, several of them run together on one line. The tool
# name is padded to 8 but can be longer (tpage-dir, subtitle-v2, build-level...)
PROGRESS = re.compile(r"\[\s*\d{1,3}%\] \[[^\[\]\n]{8,24}\] (?: {6}|\d+\.\d{3} ).{0,70}")
MARKER_BLOCK_MAX = 40  # a marker's fake error block is ~25 lines


class LogFilter:
    """What goes to the log file (the reply channel still gets everything): no (mi) progress
    entries (one per target, twice: ~90% of the log), and each marker's ~25-line fake
    "Compilation Error" block as one "-- DEVMARK-... --" line. Real errors pass untouched."""

    def __init__(self, write):
        self.write = write
        self.buf = ""
        self.dashes = None     # a dashed line held back: a marker block may start with it
        self.skip = 0          # > 0: inside a marker block (lines left before giving up)
        self.held = []         # the block's lines: written after all if it never ends

    def feed(self, s):
        self.buf += s
        *lines, self.buf = self.buf.split("\n")
        out = []
        for line in lines:
            self._line(line, out)
        if out:
            self.write("".join(out))

    def flush(self):
        out = []
        self._release(out)
        if self.dashes is not None:
            out.append(self.dashes + "\n")
            self.dashes = None
        if self.buf:
            out.append(self.buf)
            self.buf = ""
        if out:
            self.write("".join(out))

    def _line(self, line, out):
        if self.skip:
            # the block ends with "[Warning] REPL Error: Compilation Error"
            if "REPL Error" in line:
                self.skip, self.held = 0, []
                return
            self.held.append(line)
            self.skip -= 1
            if not self.skip:
                # no end in sight: not a marker block after all, keep its lines
                self._release(out)
            return
        if PROGRESS.search(line):
            line = PROGRESS.sub("", line)
            if not line.strip():
                return
        held, self.dashes = self.dashes, None
        if held is not None:
            m = re.search(r'\(error "(DEVMARK-[\w-]+)"\)', line)
            if line.startswith("From object") and m:
                # (a prompt may lead the dashed line: "g > -----...")
                out.append(f"{held.rstrip('-')}-- {m.group(1)} --\n")
                self.skip = MARKER_BLOCK_MAX
                return
            out.append(held + "\n")
        if line.endswith("-" * 40):
            self.dashes = line
            return
        out.append(line + "\n")

    def _release(self, out):
        if self.held:
            out.append("(dev_goalc: the marker block had no end, its lines follow)\n")
            out.extend(held + "\n" for held in self.held)
        self.skip, self.held = 0, []


class Goalc:
    def __init__(self, args):
        self.args = args
        self.n = args.instance
        self.nrepl_port = 8181 + instance_offset(self.n)
        self.out = []          # stripped text chunks
        self.size = 0          # total chars received
        self.text = ""         # all output (kept: goalc's output over a session is a few MB at most)
        self.cv = threading.Condition()
        self.lock = threading.Lock()   # one form at a time
        self.sock = None
        self.mark = 0
        self.mi_state = "none"
        self.connected = False
        self.loaded = {}       # rel path -> sha1 of the content the game has (after (mi) or (ml))
        self.log = open(args.log, "a", encoding="utf-8", newline="\n")
        self.log_filter = LogFilter(self._write_log)
        env = dict(os.environ)
        if self.n > 0:
            env["JAK_ENHANCED_INSTANCE"] = str(self.n)
        else:
            env.pop("JAK_ENHANCED_INSTANCE", None)
        exe = os.path.join(args.bin, "goalc.exe")
        self.proc = subprocess.Popen(
            [exe, "--user-auto", "--game", GAME, "--proj-path", args.proj],
            cwd=args.proj, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, creationflags=0x08000000)  # CREATE_NO_WINDOW
        self._job()
        threading.Thread(target=self._reader, daemon=True).start()

    def _job(self):
        """goalc dies with this process (else a goalc with a closed stdin spins a core)."""
        try:
            k32 = ctypes.windll.kernel32
            job = k32.CreateJobObjectW(None, None)

            class LIMIT(ctypes.Structure):
                _fields_ = [("a", ctypes.c_int64), ("b", ctypes.c_int64), ("flags", ctypes.c_uint32),
                            ("c", ctypes.c_size_t), ("d", ctypes.c_size_t), ("e", ctypes.c_uint32),
                            ("f", ctypes.c_size_t), ("g", ctypes.c_uint32), ("h", ctypes.c_uint32)]

            class EXT(ctypes.Structure):
                _fields_ = [("basic", LIMIT), ("io", ctypes.c_uint64 * 6), ("pm", ctypes.c_size_t),
                            ("jm", ctypes.c_size_t), ("ppm", ctypes.c_size_t), ("pjm", ctypes.c_size_t)]

            info = EXT()
            info.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info))
            k32.AssignProcessToJobObject(job, ctypes.c_void_p(int(self.proc._handle)))
            self._job_handle = job
        except Exception as e:  # not fatal: the stop op still closes goalc
            self._write_log(f"[dev_goalc] job object failed: {e}\n")

    def _write_log(self, s):
        self.log.write(s)
        self.log.flush()

    def _reader(self):
        fd = self.proc.stdout.fileno()
        while True:
            try:
                b = os.read(fd, 65536)
            except OSError:
                b = b""
            if not b:
                break
            s = ANSI.sub("", b.decode("utf-8", "replace")).replace("\r", "")
            self.log_filter.feed(s)
            with self.cv:
                self.text += s
                self.size += len(s)
                self.cv.notify_all()
        self.log_filter.flush()
        with self.cv:
            self.cv.notify_all()

    def alive(self):
        return self.proc.poll() is None

    def _send_raw(self, form):
        data = form.encode()
        msg = struct.pack("<II", len(data), EVAL) + data
        for attempt in range(2):
            try:
                if self.sock is None:
                    self.sock = socket.create_connection(("127.0.0.1", self.nrepl_port), timeout=5)
                self.sock.sendall(msg)
                return
            except OSError:
                try:
                    self.sock.close()
                except Exception:
                    pass
                self.sock = None
                if attempt:
                    raise

    def wait_nrepl(self, timeout=60):
        end = time.time() + timeout
        while time.time() < end and self.alive():
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.nrepl_port), timeout=2)
                return True
            except OSError:
                time.sleep(0.2)
        return False

    def eval(self, form, timeout=120):
        """Run one form; returns (ok, ms, output, prompt). Serialized."""
        with self.lock:
            return self._eval(form, timeout)

    def _eval(self, form, timeout):
        t0 = time.time()
        self.mark += 1
        tag = f"DEVMARK-{os.getpid()}-{self.mark}"
        with self.cv:
            start = self.size
        self._send_raw(form)
        self._send_raw(f'(seval (error "{tag}"))')
        end = t0 + timeout
        # the marker's block ends with "[Warning] REPL Error: ...\n": wait for all of it, so its tail
        # doesn't land in the next form's output
        done = re.compile(re.escape(tag) + r".*?REPL Error[^\n]*\n", re.S)
        with self.cv:
            while not done.search(self.text, start) and time.time() < end and self.alive():
                self.cv.wait(0.05)
            seg = self.text[start:]
        ms = int((time.time() - t0) * 1000)
        if tag not in seg:
            return False, ms, seg[-4000:] + ("\n[dev_goalc] goalc exited" if not self.alive()
                                             else f"\n[dev_goalc] timeout after {timeout}s"), ""
        out = seg[:seg.index(tag)]
        # the marker's block starts with a dashed line + "From object" (printed through the logger,
        # so in order); goalc's prompts come through another buffer, late and out of order: they
        # say nothing reliable about the listener, the text does
        cut = out.rfind("-----------------------------------------\nFrom object")
        if cut >= 0:
            out = out[:cut]
        prompts = PROMPTS.findall(out)
        prompt = prompts[-1] if prompts else ""
        out = PROMPTS.sub("", out)
        if "Got version" in out or "already connected" in out:
            self.connected = True
        if any(s in out for s in ("Failed to connect", "Failed to get version", "listener isn't connected",
                                  "Runtime is not responding", "Not connected")):
            self.connected = False
        lines = [l for l in out.split("\n") if l.strip() and "[nREPL:" not in l and l.strip() != "[?2004h"]
        out = "\n".join(lines)
        bad = ("Compilation Error" in out or "REPL Error" in out or "Compiler Fatal" in out
               or "Runtime is not responding" in out or "Read Error" in out)
        return not bad, ms, out, prompt


# ---- reload helpers --------------------------------------------------------------------------
def git(proj, *a):
    r = subprocess.run(["git", "-C", proj, *a], capture_output=True, text=True, encoding="utf-8",
                       errors="replace", creationflags=0x08000000)
    return r.stdout


def changed_files(proj):
    """goal_src/jak3 files changed against HEAD, plus untracked ones."""
    out = git(proj, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--", f"goal_src/{GAME}")
    names, parts, i = set(), out.split("\0"), 0
    while i < len(parts):
        e = parts[i]
        i += 1
        if len(e) < 4:
            continue
        if e[0] in "RC":   # rename/copy: the old name follows
            i += 1
        names.add(e[3:])
    return sorted(n for n in names if os.path.isfile(os.path.join(proj, n)))


def sha1(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


TYPE_RISK = re.compile(r"^[+-]\s*\((deftype|defenum|define-extern|declare-type)\b", re.M)
MACRO_RISK = re.compile(r"^[+-]\s*\((defmacro|defconstant|desfun)\b", re.M)


def risk(rel, diff):
    """Why a file can't simply be hot-loaded ('' = fine)."""
    if rel.endswith(".gp") or rel.endswith(".gs"):
        return "build/macro file"
    if rel.endswith("-h.gc"):
        return "header (type layouts)"
    if TYPE_RISK.search(diff):
        return "type definition changed"
    return ""


ERR_LOC = re.compile(r"(goal_src[\\/][^\s:]+\.g[cs]):(\d+)")
ERR_LINE = re.compile(r"Line (\d+)", re.I)


def first_error(out, rel):
    """'file:line message' from goalc's error text."""
    lines = out.split("\n")
    msg = ""
    for i, l in enumerate(lines):
        if "Compilation Error" in l and i + 1 < len(lines):
            msg = lines[i + 1].strip()
            break
        if "REPL Error" in l or "Read Error" in l:
            msg = l.strip()
            break
    if not msg:
        msg = next((l.strip() for l in lines if "rror" in l), lines[-1].strip() if lines else "?")
    loc = ""
    m = ERR_LOC.search(out)
    if m:
        loc = f"{m.group(1).replace(os.sep, '/')}:{m.group(2)}"
    else:
        m = ERR_LINE.search(out)
        loc = f"{rel}:{m.group(1)}" if m else rel
    return loc, msg[:300]


class Daemon:
    def __init__(self, args):
        self.args = args
        self.g = Goalc(args)
        self.t_start = time.time()
        self.state_file = os.path.join(os.path.dirname(args.log), f"goalc_i{args.instance}.json")

    def write_state(self):
        st = {"pid": os.getpid(), "goalc_pid": self.g.proc.pid, "instance": self.args.instance,
              "port": self.args.port, "log": self.args.log, "bin": self.args.bin, "mi": self.g.mi_state}
        with open(self.state_file, "w", encoding="utf-8") as f:
            json.dump(st, f)

    def run_mi(self):
        self.g.mi_state = "running"
        self.write_state()
        t0 = time.time()
        ok, ms, out, _ = self.g.eval("(mi)", timeout=1800)
        self.g.mi_state = ("done" if ok else "failed") + f" {ms / 1000:.1f}s"
        if ok:
            # the next boot's code = the tree as it is now
            for rel in changed_files(self.args.proj):
                self.g.loaded[rel] = sha1(os.path.join(self.args.proj, rel))
        self.write_state()
        self.g._write_log(f"\n[dev_goalc] (mi) {self.g.mi_state}\n")
        return ok, ms, out

    def game_listening(self):
        """Is a gk listening on this instance's port? Tested by binding it, never by connecting:
        gk accepts exactly one listener connection per run, a probe would use it up."""
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", 8114 + instance_offset(self.args.instance)))
            return False
        except OSError:
            return True
        finally:
            s.close()

    def connect(self):
        # (lt) against no game retries for a long time (each refused connect ~2 s on Windows)
        if not self.game_listening():
            self.g.connected = False
            return False, "no game listening"
        ok, ms, out, prompt = self.g.eval("(lt)", timeout=30)
        return self.g.connected, out

    def reload(self, req):
        proj = self.args.proj
        notes = []
        files = req.get("files") or []
        auto = not files
        if auto:
            # differs from HEAD, or was hot-loaded and changed since (e.g. reverted to HEAD)
            files = sorted(set(changed_files(proj)) | set(self.g.loaded))
            files = [f for f in files if os.path.isfile(os.path.join(proj, f))]
            if not req.get("all"):
                files = [f for f in files if self.g.loaded.get(f) != sha1(os.path.join(proj, f))]
        files = [f.replace("\\", "/") for f in files]
        tried_lt = False
        if not self.g.connected or req.get("lt"):
            tried_lt = True
            self.connect()
        results = []
        for rel in files:
            path = os.path.join(proj, rel)
            if not rel.startswith(f"goal_src/{GAME}/") or not os.path.isfile(path):
                results.append({"file": rel, "status": "SKIP", "msg": "not a goal_src/%s file" % GAME})
                continue
            diff = git(proj, "diff", "-U0", "HEAD", "--", rel)
            why = risk(rel, diff)
            if why and not req.get("force"):
                results.append({"file": rel, "status": "NEEDS", "msg": f"mi+restart ({why}); -Force loads it anyway"})
                continue
            ok, ms, out, _ = self.g.eval(f'(ml "{rel}")', timeout=req.get("timeout", 120))
            if ok and not tried_lt and (not self.g.connected or "listener isn't connected" in out):
                # the game was restarted since the last load: reconnect and load again
                tried_lt = True
                if self.connect()[0]:
                    ok, ms2, out, _ = self.g.eval(f'(ml "{rel}")', timeout=req.get("timeout", 120))
                    ms += ms2
            if ok:
                self.g.loaded[rel] = sha1(path)
                r = {"file": rel, "status": "OK", "ms": ms,
                     "loaded": self.g.connected and "couldn't load" not in out}
                if MACRO_RISK.search(diff):
                    r["msg"] = "macro/constant changed: files that use it need (mi)"
                results.append(r)
            else:
                loc, msg = first_error(out, rel)
                results.append({"file": rel, "status": "ERROR", "ms": ms, "loc": loc, "msg": msg})
        if not self.g.connected:
            notes.append("NOT-CONNECTED: no game on the listener port (or it is still booting); "
                         "files are compiled and written only")
        if req.get("mi"):
            threading.Thread(target=self.run_mi, daemon=True).start()
            notes.append("(mi) started in the background: engine files cascade, 25-70 s")
        ok = all(r["status"] in ("OK", "SKIP") for r in results)
        return {"ok": ok, "results": results, "notes": notes, "connected": self.g.connected}

    def handle(self, req):
        op = req.get("op")
        if op == "status":
            return {"ok": True, "pid": os.getpid(), "goalc_pid": self.g.proc.pid, "alive": self.g.alive(),
                    "instance": self.args.instance, "mi": self.g.mi_state, "connected": self.g.connected,
                    "log": self.args.log, "bin": self.args.bin, "proj": self.args.proj,
                    "uptime": int(time.time() - self.t_start)}
        if op == "eval":
            ok, ms, out, prompt = self.g.eval(req["form"], timeout=req.get("timeout", 120))
            return {"ok": ok, "ms": ms, "out": out, "prompt": prompt, "connected": self.g.connected}
        if op == "reload":
            return self.reload(req)
        if op == "mi":
            if self.g.mi_state == "running":
                return {"ok": True, "note": "already running"}
            if req.get("wait"):
                ok, ms, out = self.run_mi()
                return {"ok": ok, "ms": ms, "out": out[-3000:]}
            threading.Thread(target=self.run_mi, daemon=True).start()
            return {"ok": True, "note": "started"}
        if op == "stop":
            threading.Thread(target=self.stop, daemon=True).start()
            return {"ok": True}
        return {"ok": False, "error": f"unknown op {op}"}

    def stop(self):
        # kill, don't (e): goalc's exit path sends a reset to a connected game
        time.sleep(0.2)
        if self.g.alive():
            self.g.proc.kill()
            try:
                self.g.proc.wait(5)
            except Exception:
                pass
        try:
            os.remove(self.state_file)
        except OSError:
            pass
        os._exit(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--instance", type=int, default=1)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--proj", required=True)
    ap.add_argument("--log", default="")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--mi", action="store_true", help="run (mi) once at start")
    ap.add_argument("--lt", action="store_true", help="(lt) after start/(mi)")
    args = ap.parse_args()
    if not args.port:
        args.port = 8185 + instance_offset(args.instance)
    if not args.log:
        args.log = os.path.join(args.proj, "local", "run", f"goalc_i{args.instance}.log")
    os.makedirs(os.path.dirname(args.log), exist_ok=True)

    # claim the control port first: one daemon per instance
    srv_sock = socket.socket()
    try:
        srv_sock.bind(("127.0.0.1", args.port))
    except OSError:
        sys.exit(f"control port {args.port} busy: a dev_goalc for instance {args.instance} already runs?")
    srv_sock.close()

    d = Daemon(args)

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            for line in self.rfile:
                if not line.strip():
                    continue
                try:
                    resp = d.handle(json.loads(line))
                except Exception as e:
                    resp = {"ok": False, "error": repr(e)}
                self.wfile.write((json.dumps(resp) + "\n").encode())
                self.wfile.flush()

    class Server(socketserver.ThreadingTCPServer):
        daemon_threads = True
        allow_reuse_address = False

    server = Server(("127.0.0.1", args.port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    d.write_state()
    if not d.g.wait_nrepl():
        d.g._write_log("[dev_goalc] goalc nREPL never came up\n")
        d.stop()

    def boot():
        if args.mi:
            d.run_mi()
        if args.lt:
            d.connect()
    threading.Thread(target=boot, daemon=True).start()
    while d.g.alive():
        time.sleep(0.5)
    d.g._write_log("[dev_goalc] goalc exited\n")
    try:
        os.remove(d.state_file)
    except OSError:
        pass
    os._exit(0)


if __name__ == "__main__":
    main()
