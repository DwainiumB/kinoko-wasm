#!/usr/bin/env python3
"""Segment-chained edit-mode search to the finish line on the GPU (tools/gpu_brute.py's EditSearch, repeatedly).

brute_force.py's segment-chaining workflow, automated: lock everything up to frame s, search the next `--seg` frames
(plus `--look` frames of lookahead that are scored but not locked, so a segment can't win by ending in a state the
next one can't use), scored by progress along the reference ghost's line, lock the first `--seg` frames of the
winner, repeat. Once the reference finishes inside the window, the last segment is scored by brute_force's
"completion" metric instead, where a finished run ranks by its time.

Starting individuals for each segment: the reference's own inputs at several frame shifts (--shifts) and the
previous segment's winner, continued.

    python tools/gpu_chain.py --start-rkg samples/tracks/luigi-circuit/runs/gpu_brute/lc_fork1250_edits.rkg --start-frame 1450 \\
        --reference samples/tracks/luigi-circuit/ghosts/LC_3lap_67.788_ref.rkg --out-dir samples/tracks/luigi-circuit/runs/gpu_chain/fork1250
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import brute_force as bf  # noqa: E402
import gpu_brute as gb  # noqa: E402

GATED = 1e8  # fitness() adds 1e9 for a failed completion / boost gate


def progress(st, s, attempt):
    """A live line every 10 generations, so the log shows where a segment is (one line per segment otherwise)."""
    if st["bestFitness"] is None or not st["running"] or st["iteration"] % 10:
        return
    cd = st.get("completionDelta")
    print(f"   segment {s} try {attempt}: gen {st['iteration']}/{st['totalIterations']}, "
          f"{st['evaluations']:,} evals, completion {cd if cd is None else f'{cd:+d}'} vs ref, "
          f"{st['iterationsSinceImprovement']} gens since improvement", flush=True)


def run_segment(a, locked, s, starts, final, attempt, log):
    """One segment from frame s. Returns (best frames, result, status)."""
    ref_len = a.ref_len
    prefix_path = os.path.join(a.out_dir, f"locked_{s:05d}.rkg")
    with open(prefix_path, "wb") as fh:
        fh.write(bf.encode_rkg(*a.combo, locked, 0))
    common = dict(binary=gb.CPU_BINARY, course=a.combo[0], character=a.combo[1], vehicle=a.combo[2],
                  prefix_rkg=prefix_path,
                  prefix_frame_count=s, min_boost_charge=0.94, max_boost_charge=0.95,
                  completion_tolerance=a.completion_tolerance,
                  iterations=a.gens * attempt, stagnation_stop_after=a.stagnation * attempt)
    if final:
        cfg = bf.SearchConfig(fitness_metric="completion", max_frames=ref_len + a.final_extra, **common)
    else:
        cfg = bf.SearchConfig(fitness_metric="progress", reference_rkg=a.reference, progress_start_frame=s,
                              max_frames=s + a.seg + a.look, speed_bonus_weight=a.speed_bonus,
                              completion_bonus_weight=a.completion_bonus, **common)
    search = gb.EditSearch(cfg, a.reference, a.shifts, a.edits * (1.5 if attempt > 1 else 1.0), None,
                           on_status=lambda st, s=s, at=attempt: progress(st, s, at),
                           extra_bases=starts, batch=a.batch, parents=a.parents,
                           seed=a.seed + s * 10 + attempt, verify_every=a.verify_every, scoring=a.scoring)
    t = time.time()
    search.start()
    search.join()
    st = search.status()
    if st["error"]:
        raise RuntimeError(f"segment {s}: {st['error']}")
    frames, result, _ = search.best_frames_snapshot()
    line = (f"segment {s}-{cfg.max_frames}{' FINAL' if final else ''} (try {attempt}): {st['iteration']} gens, "
            f"{st['evaluations']:,} evals, {time.time() - t:.0f}s, fitness {st['bestFitness']:.2f}, "
            + (f"finished {result['timeMs']} ms" if result["finished"] else
               f"completion {result['completion']} ({st['completionDelta']:+d} vs ref)" if not final else
               f"unfinished, completion {result['completion']}")
            + (f", progress {st['progressDelta']:+.1f}" if st.get("progressDelta") is not None else "")
            + f", speed {result['speed']:.1f}, CPU checks {st['cpuChecks']}")
    print(line, flush=True)
    log.append({"start": s, "end": cfg.max_frames, "final": final, "attempt": attempt, "status": st,
                "result": result})
    return frames, result, st


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--start-rkg", required=True, help="run whose frames before --start-frame are locked")
    ap.add_argument("--start-frame", type=int, required=True)
    ap.add_argument("--reference", required=True, help="finished ghost: scoring line, and the edit bases")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--seg", type=int, default=200, help="frames locked per segment")
    ap.add_argument("--look", type=int, default=60, help="extra scored-but-not-locked frames per segment")
    ap.add_argument("--shifts", default="-4,-3,-2,-1,0,1,2,3,4", help="reference frame shifts used as bases")
    ap.add_argument("--gens", type=int, default=40, help="generations per segment (doubled on a retry)")
    ap.add_argument("--stagnation", type=int, default=12, help="stop a segment after this many flat generations")
    ap.add_argument("--edits", type=float, default=2.0, help="mean edits per child")
    ap.add_argument("--batch", type=int, default=2048)
    ap.add_argument("--parents", type=int, default=4)
    ap.add_argument("--speed-bonus", type=float, default=5.0)
    ap.add_argument("--completion-bonus", type=float, default=10.0)
    ap.add_argument("--completion-tolerance", type=int, default=1,
                    help="how far behind the reference's completion a segment may end (brute_force's gate); a "
                         "looser gate lets the chain trade a little at one checkpoint for the finish time")
    ap.add_argument("--scoring", choices=["lexi", "brute"], default="lexi",
                    help="lexi (default): gate-failing candidates ranked by completion, projection capped at the "
                         "reference line's end (see gpu_brute.GpuBruteForceSearch._fitness); brute: fitness() as is")
    ap.add_argument("--final-extra", type=int, default=250, help="frames allowed past the reference's finish")
    ap.add_argument("--verify-every", type=int, default=20)
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()
    a.shifts = [int(x) for x in a.shifts.split(",")]
    os.makedirs(a.out_dir, exist_ok=True)

    c, ch, v, ref = bf.decode_rkg(a.reference)
    a.combo = (c, ch, v)  # the reference's course / character / vehicle; the start run must match
    if bf.decode_rkg(a.start_rkg)[:3] != a.combo:
        sys.exit(f"--start-rkg is {bf.decode_rkg(a.start_rkg)[:3]}, the reference is {a.combo}")
    a.ref_len = len(ref)  # a finished ghost's frame count = its finish frame
    locked = bf.decode_rkg(a.start_rkg)[3][:a.start_frame]
    s = a.start_frame
    prev_best = bf.decode_rkg(a.start_rkg)[3]
    log = []
    t0 = time.time()
    print(f"reference finishes at frame {a.ref_len}; chaining from frame {s} in {a.seg}+{a.look}-frame segments",
          flush=True)
    result = None
    while True:
        final = s + a.seg + a.look >= a.ref_len - 30
        best = None  # (fitness, frames, result, status): the better attempt wins, not the last one
        for attempt in (1, 2):
            # a retry also starts from the first attempt's winner, so it can only match or improve on it
            starts = [f for f in (prev_best, best[1] if best else None) if f]
            frames, result, st = run_segment(a, locked, s, starts, final, attempt, log)
            if best is None or st["bestFitness"] < best[0]:
                best = (st["bestFitness"], frames, result, st)
            if final or best[0] < GATED:
                break
        _fit, frames, result, st = best
        if not (final or best[0] < GATED):
            if a.scoring != "lexi":
                print(f"segment {s} still fails the completion/boost gate after a retry -- stopping the chain here")
                break
            # lexi scoring ranks gate-failing candidates by the game's own completion, so the best one is still the
            # furthest-along run: keep going and let the finish time decide
            print(f"segment {s} is behind the gate after a retry -- continuing with the furthest-along candidate "
                  f"(completion {result['completion']})", flush=True)
        prev_best = frames
        if final:
            break
        locked = frames[:s + a.seg]
        s += a.seg
        with open(os.path.join(a.out_dir, "chain_log.json"), "w") as fh:
            json.dump(log, fh, indent=1, default=float)

    with open(os.path.join(a.out_dir, "chain_log.json"), "w") as fh:
        json.dump(log, fh, indent=1, default=float)
    if not (result and result["finished"]):
        print(f"no finish (stopped at frame {s}); total {time.time() - t0:.0f}s")
        return 1
    out = os.path.join(a.out_dir, f"chain_final_{result['timeMs']}.rkg")
    with open(out, "wb") as fh:
        fh.write(bf.encode_rkg(*a.combo, frames[:result["frames"]], result["timeMs"]))
    # independent check: the written file through the CPU build, to the finish line
    written = bf.decode_rkg(out)[3]
    cpu = bf.run_kinoko(gb.CPU_BINARY, bf.encode_task(*a.combo, written, len(written) + 300))
    ok = cpu["finished"] and cpu["timeMs"] == result["timeMs"]
    ref_ms = int(bf.run_kinoko(gb.CPU_BINARY, bf.encode_task(*a.combo, ref, len(ref) + 300))["timeMs"])
    print(f"\nFINISH {result['timeMs'] / 1000:.3f}s vs reference {ref_ms / 1000:.3f}s "
          f"({(result['timeMs'] - ref_ms) / 1000:+.3f}s); wrote {out}")
    print(f"CPU replay of the written .rkg: {'MATCH' if ok else 'MISMATCH'} ({cpu['timeMs']} ms); "
          f"total {time.time() - t0:.0f}s")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
