#!/usr/bin/env python3
"""Fix a repeated per-lap trick (e.g. SNES Mario Circuit 3's block-corner clip) in a finished run, lap by lap.

For each lap: switch the run onto the reference's line --lead frames before the reference reaches the trick (at the
point of the run nearest to the reference's position there), search that stretch with edit mode (tools/gpu_brute.py)
until it matches the reference through the trick, then finish the race with the graft beam search
(tools/gpu_beam.py). The next lap starts from that result. All output streams to one log.

    python tools/gpu_clip_fix.py --run samples/tracks/snes-mario-circuit-3/runs/gpu_beam/clip_best.rkg \\
        --reference samples/tracks/snes-mario-circuit-3/ghosts/1185.rkg --trick-frames 2761 4290 \\
        --ghosts samples/tracks/snes-mario-circuit-3/ghosts/{1185,1042,643,38}.rkg \\
        --out-dir samples/tracks/snes-mario-circuit-3/runs/clip_fix
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402

import brute_force as bf  # noqa: E402
from kinoko_search import KinokoSearch  # noqa: E402

CPU = os.path.join(ROOT, "build-gpu-spike", "kinoko_host.exe")


def positions(frames, combo, n):
    """(x, z, completion) after each of the first n frames, on the CPU."""
    out = []
    with KinokoSearch(CPU, *combo) as k:
        c = k.cols
        px, pz, ci = c.index("px"), c.index("pz"), c.index("completion")
        for f, i in enumerate(frames[:n]):
            r = k.step(i[0], i[2], i[3], i[1])
            out.append((r[px], r[pz], r[ci]))
    return np.array(out)


def stream(cmd, log):
    """Run a tool, copying its output line by line into the log (and stdout)."""
    log.write("$ " + " ".join(f'"{c}"' if " " in c else c for c in cmd) + "\n")
    log.flush()
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    for line in p.stdout:
        if "warn" in line.lower():
            continue
        log.write(line)
        log.flush()
        print(line, end="", flush=True)
    return p.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", required=True, help="finished run to fix")
    ap.add_argument("--reference", required=True, help="ghost that does the trick every lap")
    ap.add_argument("--trick-frames", type=int, nargs="+", required=True,
                    help="frames at which the REFERENCE reaches the trick on each lap to fix")
    ap.add_argument("--ghosts", nargs="+", required=True, help="graft sources for the beam search")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--lead", type=int, default=230, help="switch onto the reference's line this many frames early")
    ap.add_argument("--after", type=int, default=31, help="edit window ends this many frames after the trick")
    ap.add_argument("--score-after", type=int, default=89, help="score this many frames after the trick")
    ap.add_argument("--iterations", type=int, default=300)
    ap.add_argument("--seed", type=int, default=0, help="added to each lap's search seed (try another on a retry)")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    log = open(os.path.join(a.out_dir, "clip_fix.log"), "a", encoding="utf-8")  # appends: several runs can share a log

    def say(msg):
        print(msg, flush=True)
        log.write(msg + "\n")
        log.flush()

    c, ch, v, ref = bf.decode_rkg(a.reference)
    combo = (c, ch, v)
    run_path = a.run
    t0 = time.time()
    R = positions(ref, combo, max(a.trick_frames) + 10)
    cpu = os.path.abspath(os.path.join(ROOT, "build-host-release", "kinoko_host.exe"))

    def finish_ms(path):
        fr = bf.decode_rkg(path)[3]
        r = bf.run_kinoko(cpu, bf.encode_task(*combo, fr, len(fr) + 300))
        return (r["timeMs"] if r["finished"] else None), r

    best_ms = finish_ms(run_path)[0]
    say(f"starting run: {best_ms / 1000:.3f}s ({run_path})")
    for lap_i, tf in enumerate(a.trick_frames, 2):
        cur = bf.decode_rkg(run_path)[3]
        k = tf - a.lead  # reference frame where we join its line
        P = positions(cur, combo, len(cur))
        # our frame nearest (x, z) to the reference at frame k, among frames with a similar completion
        cand = np.nonzero(np.abs(P[:, 2] - R[k - 1, 2]) < 0.02)[0]
        j = int(cand[np.argmin((P[cand, 0] - R[k - 1, 0]) ** 2 + (P[cand, 1] - R[k - 1, 1]) ** 2)]) + 1
        dist = float(np.hypot(P[j - 1, 0] - R[k - 1, 0], P[j - 1, 1] - R[k - 1, 1]))
        say(f"\n=== lap {lap_i}: reference reaches the trick at frame {tf}; joining its line at its frame {k} = our "
            f"frame {j} ({dist:.0f} units apart, completion ours {P[j - 1, 2]:.4f} vs ref {R[k - 1, 2]:.4f})")
        merged = cur[:j] + ref[k:]
        base = os.path.join(a.out_dir, f"lap{lap_i}_base.rkg")
        open(base, "wb").write(bf.encode_rkg(*combo, merged, 0))
        win_end, score_at = j + (tf - k) + a.after, j + (tf - k) + a.score_after
        clip = os.path.join(a.out_dir, f"lap{lap_i}_clip.rkg")
        say(f"--- lap {lap_i} trick search: edit frames {j}-{win_end}, scored at {score_at} against the reference")
        rc = stream([sys.executable, "-u", "tools/gpu_brute.py", "--course", str(c), "--character", str(ch),
                     "--vehicle", str(v), "--fitness-metric", "progress", "--reference-rkg", a.reference,
                     "--progress-start-frame", str(j), "--max-frames", str(score_at), "--prefix-rkg", run_path,
                     "--prefix-frame-count", str(j), "--edit-base-rkg", base, "--edit-base-shifts=-2,-1,0,1,2",
                     "--edit-window", f"{j}-{win_end}", "--edits-per-child", "3", "--min-boost-charge", "0.94",
                     "--max-boost-charge", "0.95", "--scoring", "lexi", "--batch", "2048", "--parents", "8",
                     "--iterations", str(a.iterations), "--stagnation-stop-after", "60", "--verify-every", "50",
                     "--seed", str(lap_i + a.seed), "--out", clip], log)
        if rc:
            say(f"trick search failed (rc {rc}); stopping")
            return 1
        best = os.path.join(a.out_dir, f"lap{lap_i}_finish.rkg")
        say(f"--- lap {lap_i}: beam search from frame {score_at} to the finish")
        rc = stream([sys.executable, "-u", "tools/gpu_beam.py", "--course", str(c), "--character", str(ch),
                     "--vehicle", str(v), "--no-policy", "--ghost-files", *a.ghosts, "--start-rkg", clip,
                     "--start-frame", str(score_at), "--beam", "256", "--work", "6200", "--lookahead", "90",
                     "--line-weight", "4e-6", "--reverse-children", "--log-every", "5", "--out", best], log)
        if rc or not os.path.exists(best):
            say(f"beam search did not produce a finished run (rc {rc}); stopping")
            return 1
        ms, r = finish_ms(best)
        if ms is not None and (best_ms is None or ms < best_ms):
            say(f"=== after lap {lap_i}: {ms / 1000:.3f}s, better -- kept (CPU replay; laps {r['lap1']}, {r['lap2']}, {r['lap3']})")
            run_path, best_ms = best, ms
        else:
            say(f"=== after lap {lap_i}: {(ms or 0) / 1000:.3f}s, NOT better than {best_ms / 1000:.3f}s -- discarded, "
                f"next lap starts from {run_path}")
    say(f"\ndone in {time.time() - t0:.0f}s; final run: {run_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
