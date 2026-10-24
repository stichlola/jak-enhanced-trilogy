"""Send GOAL forms to a running goalc REPL through its nREPL socket (port 8181).

  python og_repl.py "(lt)" "(start 'play (get-continue-by-name *game-info* \"wascitya-start\"))"

Each argument is one form; the REPL prints the result in its own console.
"""
import socket
import struct
import sys
import time

EVAL = 10


def send(form, port=8181, host="127.0.0.1", retry_for=180):
    """The REPL doesn't take connections while it's busy (e.g. during (mi)): keep trying."""
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


def wait_for_server(port=8181, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=2).close()
            return True
        except OSError:
            time.sleep(1)
    return False


if __name__ == "__main__":
    if not wait_for_server():
        sys.exit("goalc nREPL not reachable on 8181")
    forms = []
    for arg in sys.argv[1:]:
        # @file: one form per line (avoids shell quoting of GOAL strings)
        if arg.startswith("@"):
            forms += [line for line in open(arg[1:], encoding="utf-8").read().splitlines() if line.strip()]
        else:
            forms.append(arg)
    for form in forms:
        send(form)
        print("sent", form)
        time.sleep(0.5)
