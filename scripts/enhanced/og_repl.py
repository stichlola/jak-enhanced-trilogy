"""Send GOAL forms to a running goalc REPL through its nREPL socket (port 8181).

  python og_repl.py "(lt)" "(start 'play (get-continue-by-name *game-info* \"wascitya-start\"))"
  python og_repl.py --instance 2 "(lt)"   # the goalc started with JAK_ENHANCED_INSTANCE=2

Each argument is one form; the REPL prints the result in its own console.
A parallel test instance (JAK_ENHANCED_INSTANCE=<n>, or --instance n) uses port 8181 + 10*n;
--port overrides both.
"""
import os
import socket
import struct
import sys
import time

EVAL = 10
BASE_PORT = 8181


def instance_port(instance=None):
    """goalc's nREPL port for a test instance (same rule as enhanced_port_offset in C++)."""
    if instance is None:
        try:
            instance = int(os.environ.get("JAK_ENHANCED_INSTANCE", "0"))
        except ValueError:
            instance = 0
    return BASE_PORT + (10 * instance if 0 < instance < 100 else 0)


def send(form, port=None, host="127.0.0.1", retry_for=180):
    """The REPL doesn't take connections while it's busy (e.g. during (mi)): keep trying."""
    port = instance_port() if port is None else port
    data = form.encode()
    end = time.time() + retry_for
    while True:
        try:
            with socket.create_connection((host, port), timeout=10) as sock:
                sock.sendall(struct.pack("<II", len(data), EVAL) + data)
                time.sleep(0.3)
            return
        except OSError:
            if time.time() > end:
                raise
            time.sleep(2)


def wait_for_server(port=None, timeout=120):
    port = instance_port() if port is None else port
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=2).close()
            return True
        except OSError:
            time.sleep(1)
    return False


if __name__ == "__main__":
    args = sys.argv[1:]
    port = instance_port()
    while args and args[0] in ("--instance", "--port"):
        flag, value, args = args[0], int(args[1]), args[2:]
        port = instance_port(value) if flag == "--instance" else value
    if not wait_for_server(port):
        sys.exit(f"goalc nREPL not reachable on {port}")
    forms = []
    for arg in args:
        # @file: one form per line (avoids shell quoting of GOAL strings)
        if arg.startswith("@"):
            forms += [line for line in open(arg[1:], encoding="utf-8").read().splitlines() if line.strip()]
        else:
            forms.append(arg)
    for form in forms:
        send(form, port)
        print("sent", form)
        time.sleep(0.5)
