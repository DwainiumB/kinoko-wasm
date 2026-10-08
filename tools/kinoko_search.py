"""Python wrapper around `kinoko_host search`: a Kinoko race that can be saved and restored.

See source/host/KSearchSystem.hh for the protocol. Snapshots copy the whole 64 MB game heap
(Host::Context), so keep the number alive in one process modest and FREE the ones you no longer need.
"""

from __future__ import annotations

import os
import subprocess

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_BINARY = os.path.join(ROOT, "build-host-release", "kinoko_host.exe")


class KinokoSearch:
    def __init__(self, binary=DEFAULT_BINARY, course=8, character=22, vehicle=32):
        env = dict(os.environ, KINOKO_FILESYSTEM_ROOT=ROOT)
        self.proc = subprocess.Popen([binary, "search", "--course", str(course), "--character", str(character),
                "--vehicle", str(vehicle)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env, cwd=ROOT)
        self.cols = self._read("COLS ").strip().split(",")
        self.end = None  # parsed END line once the race has finished

    def _read(self, tag):
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("kinoko_host search exited unexpectedly")
            if line.startswith(tag):
                return line[len(tag):]

    def _send(self, text):
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()

    def step(self, buttons, stick_x, stick_y, trick) -> np.ndarray:
        """Simulate one frame and return the state row (see source/host/FrameState.cc)."""
        self._send(f"STEP {int(buttons)} {stick_x:.9g} {stick_y:.9g} {int(trick)}")
        row = np.array(self._read("ROW ").strip().split(","), dtype=np.float64)
        if row[self.cols.index("stage")] >= 4:
            self.end = dict((k, int(v)) for k, v in (kv.split("=") for kv in self._read("END ").split()))
        return row

    def save(self, snap_id: int):
        self._send(f"SAVE {snap_id}")
        self._read("OK")

    def load(self, snap_id: int):
        self._send(f"LOAD {snap_id}")
        self._read("OK")
        self.end = None

    def free(self, snap_id: int):
        self._send(f"FREE {snap_id}")
        self._read("OK")

    def close(self):
        try:
            self._send("QUIT")
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
