#!/usr/bin/env python3
"""Prune a finished searched run back toward the reference ghost: undo every edit that doesn't change the result.

The run's frames from --from onward are compared with the reference's, aligned per --align-window frames to the
reference shift (within +-max-shift) that matches best there (a chained search can drift a frame or two against the
reference from one segment to the next). Each contiguous run of differing frames is one "edit". Every round:

  1. each remaining edit is reverted on its own (frames set back to the aligned reference's), all evaluated to the
     finish line in one GPU batch; an edit whose revert keeps the finish time <= the current one is "removable";
  2. removable edits are applied cumulatively (first k of them, for every k, again one batch) and the largest k that
     still finishes no slower is kept.

Rounds repeat until nothing more can be removed. The pruned run is written and replayed on the CPU build.

    python tools/gpu_prune.py --run samples/tracks/luigi-circuit/runs/gpu_chain/fork1250/chain_final_67xxx.rkg \\
        --reference samples/tracks/luigi-circuit/ghosts/LC_3lap_67.788_ref.rkg --from 1250 --out samples/tracks/luigi-circuit/runs/gpu_chain/fork1250/pruned.rkg
"""

from __future__ import annotations

import argparse
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import brute_force as bf  # noqa: E402
import gpu_brute as gb  # noqa: E402


def as_ind(frames, lo, hi):
    """Frames lo..hi-1 -> EditSearch individual (4, T) int16."""
    f = [frames[i] if i < len(frames) else gb.NEUTRAL for i in range(lo, hi)]
    return np.array([[x[0] for x in f], [x[1] for x in f], [bf._stick_raw(x[2]) for x in f],
                     [bf._stick_raw(x[3]) for x in f]], np.int16)


def aligned_reference(run, ref, lo, hi, window, max_shift):
    """For each frame lo..hi-1, the reference frame it should be compared with: ref[i + s], with s the shift that
    matches the run best over the window containing i."""
    out = []
    for w0 in range(lo, hi, window):
        w1 = min(w0 + window, hi)
        best = min(range(-max_shift, max_shift + 1),
                   key=lambda s: (sum(run[i] != (ref[i + s] if 0 <= i + s < len(ref) else gb.NEUTRAL)
                                      for i in range(w0, w1)), abs(s)))
        out += [ref[i + best] if 0 <= i + best < len(ref) else gb.NEUTRAL for i in range(w0, w1)]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", required=True, help="finished run to prune")
    ap.add_argument("--reference", required=True, help="ghost to revert toward")
    ap.add_argument("--from", dest="lo", type=int, required=True, help="frames before this are left alone")
    ap.add_argument("--out", required=True)
    ap.add_argument("--align-window", type=int, default=50)
    ap.add_argument("--max-shift", type=int, default=6)
    ap.add_argument("--merge-gap", type=int, default=0,
                    help="treat differing runs closer than this many frames as one edit")
    ap.add_argument("--batch", type=int, default=1024)
    a = ap.parse_args()

    c, ch, v, run = bf.decode_rkg(a.run)
    combo = (c, ch, v)
    ref = bf.decode_rkg(a.reference)[3]
    t0 = time.time()
    cfg = bf.SearchConfig(binary=gb.CPU_BINARY, course=c, character=ch, vehicle=v, fitness_metric="completion",
                          prefix_rkg=a.run, prefix_frame_count=a.lo, max_frames=len(run) + 120, iterations=0)
    # EditSearch only for its prepared base slot + evaluate(); its own search loop is not used
    s = gb.EditSearch(cfg, a.reference, (0,), batch=a.batch)
    s.prepare()
    hi = cfg.max_frames
    cur = list(run[:hi]) + [gb.NEUTRAL] * max(0, hi - len(run))  # padded past the finish
    target = aligned_reference(cur, ref, a.lo, hi, a.align_window, a.max_shift)  # frame lo + k -> target[k]

    def evaluate(frame_lists):
        inds = np.stack([as_ind(f, a.lo, hi) for f in frame_lists])
        return s.evaluate(inds)

    r0 = evaluate([cur])[0]
    if not r0["finished"]:
        sys.exit("the run doesn't finish on the GPU -- nothing to prune against")
    cur_ms = r0["timeMs"]
    print(f"run finishes in {cur_ms} ms; reverting edits from frame {a.lo} toward {os.path.basename(a.reference)}")

    def edits_of(frames):
        diff = [i for i in range(a.lo, hi) if frames[i] != target[i - a.lo]]
        runs = []
        for i in diff:
            if runs and i - runs[-1][1] <= a.merge_gap:
                runs[-1][1] = i + 1
            else:
                runs.append([i, i + 1])
        return [tuple(r) for r in runs]

    def reverted(frames, edits):
        f = list(frames)
        for e0, e1 in edits:
            for i in range(e0, e1):
                f[i] = target[i - a.lo]
        return f

    rnd = 0
    while True:
        rnd += 1
        edits = edits_of(cur)
        singles = evaluate([reverted(cur, [e]) for e in edits]) if edits else []
        removable = [e for e, r in zip(edits, singles) if r["finished"] and r["timeMs"] <= cur_ms]
        print(f"round {rnd}: {len(edits)} edits ({sum(e1 - e0 for e0, e1 in edits)} frames), "
              f"{len(removable)} individually removable", flush=True)
        if not removable:
            break
        # cumulative: first k removable edits, for every k; keep the largest k that is still no slower
        cums = evaluate([reverted(cur, removable[:k]) for k in range(1, len(removable) + 1)])
        ok = [k for k, r in enumerate(cums, 1) if r["finished"] and r["timeMs"] <= cur_ms]
        k = max(ok) if ok else 1
        if not ok:
            k, r = 1, singles[edits.index(removable[0])]
        else:
            r = cums[k - 1]
        cur = reverted(cur, removable[:k])
        cur_ms = r["timeMs"]
        print(f"   removed {k} edits -> {cur_ms} ms", flush=True)

    left = edits_of(cur)
    finish_frame = evaluate([cur])[0]["frames"]
    with open(a.out, "wb") as fh:
        fh.write(bf.encode_rkg(*combo, cur[:finish_frame], cur_ms))
    written = bf.decode_rkg(a.out)[3]
    cpu = bf.run_kinoko(gb.CPU_BINARY, bf.encode_task(*combo, written, len(written) + 300))
    ok = cpu["finished"] and cpu["timeMs"] == cur_ms
    print(f"\npruned: {cur_ms} ms (was {r0['timeMs']} ms), {len(left)} edits left "
          f"({sum(e1 - e0 for e0, e1 in left)} frames differ from the aligned reference); wrote {a.out}")
    print(f"CPU replay of the written .rkg: {'MATCH' if ok else 'MISMATCH'} ({cpu['timeMs']} ms); "
          f"{time.time() - t0:.0f}s")
    for e0, e1 in left:
        print(f"   kept edit frames {e0}-{e1 - 1}: " + "; ".join(
            f"{i}: {cur[i]} (ref {target[i - a.lo]})" for i in range(e0, min(e1, e0 + 3))) + (" ..." if e1 - e0 > 3 else ""))
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
