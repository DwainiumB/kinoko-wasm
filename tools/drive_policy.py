#!/usr/bin/env python3
"""Closed-loop test: let a trained policy drive a race in Kinoko and report how it did.

    python tools/drive_policy.py --policy samples/data/dumps/lc_policy_path.npz --export out.rkg --verify

`kinoko_host drive` runs the physics and exchanges one line per frame with this process (protocol in
source/host/KDriveSystem.hh). The countdown is scripted (accelerate is held from the best
start-boost frame found by brute_force.find_best_start_boost_frame); from the GO frame on, every
input comes from the policy, which sees exactly the columns that tools/dump_ghosts.py produced for
its training data. A finished run can be exported as an .rkg and replayed through `kinoko_host
replay`, which proves the exported inputs reproduce the same race.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from brute_force import GO_FRAME, encode_rkg, find_best_start_boost_frame  # noqa: E402
from policy_net import Policy, state_features  # noqa: E402

DEFAULT_BINARY = os.path.join(ROOT, "build-host-release", "kinoko_host.exe")


class KinokoRace:
    """One race in a `kinoko_host drive` subprocess."""

    def __init__(self, binary, course=8, character=22, vehicle=32, max_frames=9000):
        env = dict(os.environ, KINOKO_FILESYSTEM_ROOT=ROOT)
        self.proc = subprocess.Popen([binary, "drive", "--course", str(course), "--character", str(character),
                "--vehicle", str(vehicle), "--maxframes", str(max_frames)], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env, cwd=ROOT)
        self.cols = self._read_tagged("COLS ").strip().split(",")
        self.end = None

    def _read_tagged(self, *tags):
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("kinoko_host drive exited unexpectedly")
            for tag in tags:
                if line.startswith(tag):
                    return line[len(tag):] if len(tags) == 1 else line

    def step(self, buttons, stick_x, stick_y, trick):
        """Apply an input for one frame. Returns the state row after the frame, or None when the race ended."""
        try:
            self.proc.stdin.write(f"{int(buttons)} {stick_x:.9g} {stick_y:.9g} {int(trick)}\n")
            self.proc.stdin.flush()
        except OSError:
            pass  # the race already ended and the process exited; its END line is still readable
        line = self._read_tagged("ROW ", "END ")
        if line.startswith("END "):
            self.end = dict(kv.split("=") for kv in line[4:].split())
            self.end = {k: int(v) for k, v in self.end.items()}
            return None
        return np.array(line[4:].strip().split(","), dtype=np.float64)

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # e.g. a loaded machine: the process did not notice stdin closing in time

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


@dataclass
class DriveResult:
    finished: bool
    time_ms: int
    completion: float  # in laps (4.0 = 3 laps done)
    frames: int
    inputs: list = field(default_factory=list)  # (buttons, trick, stickX, stickY) per absolute frame


def snap_stick(v):
    """The RKG format and the pad both use a 15-step stick grid; snap to the exact k/7 value."""
    return round(float(v) * 7) / 7


def drive(policy: Policy, binary=DEFAULT_BINARY, start_frame=None, course=8, character=22, vehicle=32,
        max_frames=9000, log_every=0):
    if start_frame is None:
        start_frame = find_best_start_boost_frame()[0]
    inputs = []
    t0 = time.time()
    with KinokoRace(binary, course, character, vehicle, max_frames) as race:
        cols = race.cols
        row = None
        frame = 0
        while True:
            if frame < GO_FRAME:
                inp = (1 if frame >= start_frame else 0, 0, 0.0, 0.0)  # buttons, trick, x, y
            else:
                feats, _ = state_features(row[None, :].astype(np.float64), cols, policy.use_prev_input)
                a = policy.act(feats)
                inp = (int(a["buttons"][0]), int(a["trick"][0]), snap_stick(a["stickX"][0]), snap_stick(a["stickY"][0]))
            inputs.append(inp)
            row = race.step(inp[0], inp[2], inp[3], inp[1])
            if row is None:
                break
            frame += 1
            if log_every and frame % log_every == 0:
                g = lambda n: row[cols.index(n)]
                print(f"  frame {frame:5d}  lap {int(g('lap'))}  completion {g('completion'):.3f}  speed {g('speed'):6.1f}"
                      f"  stick {g('stickX'):+.2f}  wall {int(g('flags')) >> 10 & 1}")
        end = race.end
    elapsed = time.time() - t0
    print(f"{end['frames']} frames in {elapsed:.1f}s")
    return DriveResult(bool(end["finished"]), end["timeMs"], end["completion"] / 10000.0, end["frames"], inputs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--policy", required=True)
    ap.add_argument("--binary", default=DEFAULT_BINARY)
    ap.add_argument("--start-frame", type=int, default=None, help="frame at which accelerate is first held")
    ap.add_argument("--course", type=int, default=8)
    ap.add_argument("--character", type=int, default=22)
    ap.add_argument("--vehicle", type=int, default=32)
    ap.add_argument("--max-frames", type=int, default=9000)
    ap.add_argument("--log-every", type=int, default=300)
    ap.add_argument("--export", help="write the run as an .rkg (only if it finished)")
    ap.add_argument("--verify", action="store_true", help="replay the exported .rkg with kinoko_host replay")
    args = ap.parse_args()

    pol = Policy.load(args.policy)
    res = drive(pol, args.binary, args.start_frame, args.course, args.character, args.vehicle, args.max_frames,
            args.log_every)

    laps = res.completion - 1.0
    print(f"\nfinished: {res.finished}   time: {res.time_ms / 1000 if res.finished else float('nan'):.3f}s   "
          f"progress: {max(laps, 0):.2f}/3 laps")

    if args.export:
        if not res.finished:
            print("not exporting: the run did not finish")
            return 1
        data = encode_rkg(args.course, args.character, args.vehicle, res.inputs, res.time_ms)
        open(args.export, "wb").write(data)
        print(f"wrote {args.export}")
        if args.verify:
            env = dict(os.environ, KINOKO_FILESYSTEM_ROOT=ROOT)
            p = subprocess.run([args.binary, "replay", "--ghost", os.path.abspath(args.export)], env=env,
                    cwd=ROOT, capture_output=True, text=True)
            print("replay verify:", "MATCH (exit 0)" if p.returncode == 0 else f"MISMATCH (exit {p.returncode})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
