#!/usr/bin/env python3
"""Can any section of a ghost be improved on its own? Edit-mode GPU search, one window at a time.

For each window [a, b): the ghost's own inputs are locked before a and kept after b; only frames a..b-1 are edited
(tools/gpu_brute.py EditSearch, base = the ghost itself), and candidates are scored at b + --look by progress along
the ghost's own path (lexi scoring, start-boost gates 0.94..0.95). A gain therefore has to survive the ghost's own
continuation for --look frames. Windows: the countdown (172..412, scored at 412 + 108) and then --width-frame windows
from 412 to the end of the ghost's real inputs (trailing neutral frames are ignored).

    python tools/gpu_window_sweep.py --ghost "samples/tracks/snes-mario-circuit-3/ghosts/rMC3_no name_0m35s856 (1).rkg"
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import brute_force as bf  # noqa: E402


def real_input_end(frames, gap=10, stray=10):
    """Frame count up to the end of a ghost's real inputs: the first run of >= `gap` empty frames after GO that is
    followed by fewer than `stray` non-empty frames (an unfinished WIP is often cut with a few stray presses after
    its real end -- e.g. a Yoshi Falls WIP that drives to 1786, then is empty, then has 2 accelerate frames)."""
    empty = [f == (0, 0, 0.0, 0.0) for f in frames]
    i = bf.GO_FRAME
    while i < len(frames):
        if empty[i]:
            j = i
            while j < len(frames) and empty[j]:
                j += 1
            if j - i >= gap and sum(1 for k in range(j, len(frames)) if not empty[k]) < stray:
                return i
            i = j
        else:
            i += 1
    return max(k for k, e in enumerate(empty) if not e) + 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ghost", required=True)
    ap.add_argument("--width", type=int, default=150)
    ap.add_argument("--look", type=int, default=100)
    ap.add_argument("--end", type=int, default=None, help="last frame to search (default: end of the real inputs)")
    ap.add_argument("--iterations", type=int, default=200)
    ap.add_argument("--stagnation", type=int, default=40)
    ap.add_argument("--batch", type=int, default=2048)
    ap.add_argument("--drift-ops", type=float, default=0.0,
                    help="fraction of edits drawn from gpu_brute's drift-structure ops (0 = off, ~0.5 typical)")
    ap.add_argument("--edits-per-child", type=float, default=2.0)
    ap.add_argument("--scoring", choices=["lexi", "tte"], default="lexi",
                    help="tte: score by estimated finish time (gpu_brute --scoring tte); the gain is then in ms, and "
                         "needs a ghost that finishes (--reference-ghost; default: the swept ghost itself)")
    ap.add_argument("--tte-height-weight", type=float, default=0.0, help="gpu_brute --tte-height-weight")
    ap.add_argument("--tte-completion-weight", type=float, default=0.0, help="gpu_brute --tte-completion-weight")
    ap.add_argument("--repair-top", type=int, default=0, help="gpu_brute --repair-top (needs --scoring tte)")
    ap.add_argument("--repair-frames", type=int, default=100, help="gpu_brute --repair-frames")
    ap.add_argument("--finish", action="store_true",
                    help="the ghost FINISHES the race: score every candidate by its real finish time (the edited run "
                         "followed by the ghost's own later inputs, to the line), no estimate and no reference. Windows "
                         "cover the whole race; with --chain a window's winner replaces the base only if it finishes "
                         "sooner. The gain is in ms")
    ap.add_argument("--graft-rkg", nargs="+", default=[], help="gpu_brute --graft-rkg")
    ap.add_argument("--graft-ops", type=float, default=0.0, help="gpu_brute --graft-ops")
    ap.add_argument("--stride", type=int, default=None,
                    help="frames between window starts (default: --width, i.e. no overlap); a smaller stride gives "
                         "overlapping windows, e.g. --width 500 --stride 250")
    ap.add_argument("--from-frame", type=int, default=None,
                    help="first frame to search: skips the countdown window and starts the 150/300-frame windows here")
    ap.add_argument("--chain", action="store_true",
                    help="each window starts from the previous window's winner (merged back onto the ghost's own later "
                         "inputs, aligned to within +-8 frames) instead of from the unedited ghost; the last merged run "
                         "is written to chain_final.rkg")
    ap.add_argument("--reference-ghost", help="finished ghost for --scoring tte (the edit base is still --ghost)")
    ap.add_argument("--parents", type=int, default=4)
    ap.add_argument("--out-dir", default=None, help="default: the track's runs/window_sweep/<ghost name>")
    a = ap.parse_args()

    c, ch, v, frames = bf.decode_rkg(a.ghost)
    last = real_input_end(frames)
    end = min(a.end or last, last)
    name = os.path.splitext(os.path.basename(a.ghost))[0]
    track_dir = os.path.dirname(os.path.dirname(os.path.abspath(a.ghost)))
    out_dir = a.out_dir or os.path.join(track_dir, "runs", "window_sweep", name.replace(" ", "_"))
    os.makedirs(out_dir, exist_ok=True)
    log = open(os.path.join(out_dir, "sweep.log"), "w", encoding="utf-8")

    windows = [] if a.from_frame else [(bf.COUNTDOWN_START_FRAME, bf.GO_FRAME + 1, 108)]
    s = a.from_frame or bf.GO_FRAME + 1
    while s < end:
        windows.append((s, min(s + a.width, end), a.look))
        s += a.stride or a.width

    def say(msg):
        print(msg, flush=True)
        log.write(msg + "\n")
        log.flush()

    say(f"{name}: course {c}, character {ch}, vehicle {v}; real inputs end at frame {last}; {len(windows)} windows")
    t0 = time.time()
    base_path = a_ghost = a.ghost
    for w0, w1, look in windows:
        score_at = min(w1 + look, last)  # can't score past the ghost's own inputs
        if a.finish:
            score_at = len(bf.decode_rkg(base_path)[3]) + 60  # the whole race, plus slack for a slower finish
        mem = subprocess.run([sys.executable, "tools/mem.py", "--min-mb", "3500"], cwd=ROOT, capture_output=True,
                             text=True)
        if mem.returncode:
            say(f"window {w0}-{w1}: SKIPPED, low RAM ({mem.stdout.strip()})")
            continue
        out = os.path.join(out_dir, f"win_{w0:05d}_{w1:05d}.rkg")
        cmd = [sys.executable, "-u", "tools/gpu_brute.py", "--course", str(c), "--character", str(ch), "--vehicle",
               str(v), "--fitness-metric", "progress", "--reference-rkg", a.reference_ghost or base_path, "--progress-start-frame", str(w0),
               "--max-frames", str(score_at), "--prefix-rkg", base_path, "--prefix-frame-count", str(w0),
               "--edit-base-rkg", base_path, "--edit-window", f"{w0}-{w1}", "--min-boost-charge", "0.94",
               "--max-boost-charge", "0.95", "--scoring", a.scoring, "--batch", str(a.batch), "--parents", str(a.parents),
               "--drift-ops", str(a.drift_ops), "--repair-top", str(a.repair_top), "--repair-frames", str(a.repair_frames),
               "--tte-height-weight", str(a.tte_height_weight),
               "--tte-completion-weight", str(a.tte_completion_weight), "--edits-per-child", str(a.edits_per_child),
               "--iterations", str(a.iterations), "--stagnation-stop-after", str(a.stagnation), "--verify-every", "25",
               "--seed", str(w0), "--out", out]
        if a.finish:  # real finish time: the completion metric ranks finished runs by time; no reference, no gates
            cmd = [sys.executable, "-u", "tools/gpu_brute.py", "--course", str(c), "--character", str(ch),
                   "--vehicle", str(v), "--fitness-metric", "completion", "--max-frames", str(score_at),
                   "--prefix-rkg", base_path, "--prefix-frame-count", str(w0), "--edit-base-rkg", base_path,
                   "--edit-window", f"{w0}-{w1}", "--scoring", "brute", "--batch", str(a.batch), "--parents",
                   str(a.parents), "--drift-ops", str(a.drift_ops), "--edits-per-child", str(a.edits_per_child),
                   "--graft-ops", str(a.graft_ops), *(["--graft-rkg", *a.graft_rkg] if a.graft_rkg else []),
                   "--iterations", str(a.iterations), "--stagnation-stop-after", str(a.stagnation),
                   "--verify-every", "25", "--seed", str(w0), "--out", out]
        say(f"--- window {w0}-{w1}, scored @{score_at}")
        p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        lines = []
        for line in p.stdout:  # streamed into the log as it happens, so progress can be watched live
            if "warn" in line.lower():
                continue
            lines.append(line.rstrip())
            log.write("   " + line)
            log.flush()
        p.wait()
        gens = [l for l in lines if l.startswith("gen ")]
        check = next((l for l in lines if "CPU replay" in l), "no CPU replay line")
        first, best = (gens[0], gens[-1]) if gens else ("", "")

        def prog(line):
            import re
            m = re.search(r"best=([\d.-]+) units", line)
            return float(m.group(1)) if m else None

        def fit(line):
            import re
            m = re.search(r"fitness=([\d.-]+)", line)
            return float(m.group(1)) if m else None

        p0, p1 = prog(first), prog(best)
        gain = f"{p1 - p0:+.2f} units" if p0 is not None and p1 is not None else "?"
        if (a.scoring == "tte" or a.finish) and fit(first) is not None and fit(best) is not None:
            gain = f"{fit(first) - fit(best):+.2f} ms faster" + ("" if a.finish else " est. finish")
        say(f"window {w0}-{w1} scored @{score_at}: gain {gain}  |  {best.split(' (')[0][4:] if best else '(no output)'}"
            f"  |  {check}" + ("" if p.returncode == 0 else f"  |  rc={p.returncode} {lines[-3:]}"))
        if a.finish and a.chain and p.returncode == 0 and os.path.exists(out) and fit(first) is not None                 and fit(best) is not None and fit(best) < fit(first) and "MATCH" in check:
            base_path = os.path.join(out_dir, f"chain_{w1:05d}.rkg")
            shutil.copy(out, base_path)
            say(f"    chained: kept the winner ({fit(first) - fit(best):.1f} ms faster) -> {base_path}")
        elif a.chain and not a.finish and p.returncode == 0 and os.path.exists(out):
            win = bf.decode_rkg(out)[3]
            orig = frames[:last]
            lo_i, hi_i = w1 + 5, min(score_at, len(win)) - 5
            best_s, best_n = 0, -1
            for sh in sorted(range(-8, 9), key=abs):
                n = sum(1 for i in range(lo_i, hi_i) if 0 <= i + sh < len(orig) and win[i] == orig[i + sh])
                if n > best_n:
                    best_s, best_n = sh, n
            merged = list(win[:score_at]) + list(orig[score_at + best_s:])
            base_path = os.path.join(out_dir, f"chain_{w1:05d}.rkg")
            open(base_path, "wb").write(bf.encode_rkg(c, ch, v, merged, 0))
            say(f"    chained: winner shifted {best_s:+d} frames against the ghost over {w1}-{score_at} "
                f"({best_n}/{max(1, hi_i - lo_i)} frames equal) -> {base_path}")
    if a.chain:
        import shutil
        if base_path != a_ghost:
            shutil.copy(base_path, os.path.join(out_dir, "chain_final.rkg"))
        say(f"chained run: {os.path.join(out_dir, 'chain_final.rkg')}")
    say(f"done in {time.time() - t0:.0f}s; outputs in {out_dir}")


if __name__ == "__main__":
    main()
