"""How much of the game heap can a race change, and where?

For each scenario: snapshot at the GO frame, play on, and ask the engine (DIRTY) how the live heap differs from the
snapshot. Reports the number of bytes that differ and the highest differing offset, which bounds the mutable state of
a race. Scenarios include real ghosts (TAS, RTA) and abusive input (wall driving, reversing, trick and mushroom spam).

    python tools/state_extent.py
"""

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from brute_force import GO_FRAME, decode_rkg  # noqa: E402
from kinoko_search import KinokoSearch  # noqa: E402


def ghost_inputs(path):
    return [(b, t, x, y) for (b, t, x, y) in decode_rkg(os.path.join(ROOT, path))[3]]


def scenarios():
    rng = np.random.default_rng(0)
    out = {}
    for name, path in (("TAS 1142", "samples/tracks/luigi-circuit/ghosts/tas/1142.rkg"), ("TAS 702", "samples/tracks/luigi-circuit/ghosts/tas/702.rkg"),
                       ("RTA WR", "samples/lc-rta-1-08-733.rkg"), ("our 68.916", "samples/tracks/luigi-circuit/runs/best/lc-spear-68916.rkg")):
        out[name] = ghost_inputs(path)
    start = [(1 if f >= 305 else 0, 0, 0.0, 0.0) for f in range(GO_FRAME)]
    out["hold left into walls"] = start + [(1, 0, -1.0, 0.0)] * 1500
    out["hold right into walls"] = start + [(1, 0, 1.0, 0.0)] * 1500
    out["reverse"] = start + [(2, 0, 0.0, 0.0)] * 1200
    out["trick + mushroom spam"] = start + [(1 | 4 | 8, int(rng.integers(0, 5)), float(rng.choice([-1.0, 0.0, 1.0])), 0.0) for _ in range(2000)]
    out["random inputs"] = start + [(int(rng.integers(0, 16)), int(rng.integers(0, 5)), float(rng.integers(-7, 8)) / 7, float(rng.integers(-7, 8)) / 7)
                                   for _ in range(3000)]
    return out


def main():
    print(f"{'scenario':26s} {'frames':>6s} {'bytes changed':>14s} {'pages':>6s} {'highest offset (KiB)':>21s}")
    worst = 0
    for name, inputs in scenarios().items():
        with KinokoSearch() as race:
            for f in inputs[:GO_FRAME]:
                race.step(f[0], f[2], f[3], f[1])
            race.save(1)
            peak_bytes = peak_pages = 0
            last = 0
            for i, f in enumerate(inputs[GO_FRAME:], 1):
                race.step(f[0], f[2], f[3], f[1])
                if race.end is not None:
                    break
                if i % 250 == 0:
                    race._send("DIRTY 1")
                    head = race._read("DIRTY ").split(" perMiB=")[0].split()
                    kv = dict(x.split("=") for x in head)
                    peak_bytes = max(peak_bytes, int(kv["bytes"]))
                    peak_pages = max(peak_pages, int(kv["pages"]))
                    last = max(last, int(kv["last"]))
            race._send("DIRTY 1")
            head = race._read("DIRTY ").split(" perMiB=")[0].split()
            kv = dict(x.split("=") for x in head)
            peak_bytes = max(peak_bytes, int(kv["bytes"]))
            peak_pages = max(peak_pages, int(kv["pages"]))
            last = max(last, int(kv["last"]))
            worst = max(worst, last)
            print(f"{name:26s} {min(i, len(inputs) - GO_FRAME):6d} {peak_bytes:14d} {peak_pages:6d} {last / 1024:21.1f}")
    print(f"\nhighest byte offset that changed in any scenario: {worst} ({worst / 1024:.1f} KiB, {worst / 2**20:.2f} MiB)")


if __name__ == "__main__":
    main()
