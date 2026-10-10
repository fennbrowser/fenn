"""Shared launch and Marionette helpers for the Fenn integration suites.

Talks to the staged browser's built-in Marionette server (protocol v3) directly,
without downloading a driver or Python packages.
"""
import json
import socket
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The browser executable; configs/mozconfig sets its name (--with-app-name).
EXE = ROOT / "build/firefox/dist/bin/fenn.exe"


def browser_descendants(parents, root_pid):
    """Include children of browser processes, excluding unrelated process trees.

    parents maps pid to (parent_pid, executable_name), from one OS snapshot.
    A process can exit between snapshots, including the launcher's parent.
    """
    found = {root_pid}
    changed = True
    while changed:
        changed = False
        for pid, (parent, _) in parents.items():
            if (parent in found and pid not in found and
                    parents.get(parent, (None, ""))[1].lower() == EXE.name.lower()):
                found.add(pid)
                changed = True
    return sorted(found)


class Marionette:
    def __init__(self, port, process):
        deadline = time.monotonic() + 45
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 1)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Firefox did not open its test port")
                time.sleep(0.1)
        self.sock.settimeout(30)
        self.sequence = 0
        greeting = self.read()
        if greeting.get("marionetteProtocol") != 3:
            raise RuntimeError(f"Unexpected protocol: {greeting}")

    def read(self):
        prefix = b""
        while not prefix.endswith(b":"):
            data = self.sock.recv(1)
            if not data:
                raise EOFError("Marionette closed")
            prefix += data
        size = int(prefix[:-1])
        payload = b""
        while len(payload) < size:
            chunk = self.sock.recv(size - len(payload))
            if not chunk:
                raise EOFError("Truncated Marionette response")
            payload += chunk
        return json.loads(payload)

    def command(self, name, parameters=None):
        self.sequence += 1
        payload = json.dumps([0, self.sequence, name, parameters or {}]).encode()
        self.sock.sendall(str(len(payload)).encode() + b":" + payload)
        while True:
            reply = self.read()
            if reply[0] != 1:
                continue
            if reply[1] != self.sequence or reply[2]:
                raise RuntimeError(f"{name}: {reply}")
            return reply[3]
