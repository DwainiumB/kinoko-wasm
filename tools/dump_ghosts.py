#!/usr/bin/env python3
"""Replay ghosts in Kinoko and dump their per-frame state and inputs for training.

For every .rkg it runs `kinoko_host replay --ghost <rkg> --dump <csv>`. The replay itself is the
correctness check: kinoko_host exits 0 only if the simulated final and lap timers equal the ones
stored in the ghost, so a ghost that desyncs is reported as FAIL and left out of the dataset.
Passing ghosts are packed into one compressed .npz:

    data        float32 (rows, len(columns)); only rows from the start of the race (stage >= 2)
    columns     column names (see KReplaySystem::dumpFrame in source/host/KReplaySystem.cc)
    ghost_idx   int32 (rows,) index into `names`
    names       ghost file names
    race_frame  int32 (rows,) frame counted from GO (0 on the first race frame)
    meta        JSON string: per ghost {name, course, vehicle, character, headerTimeMs, frames}

Row t holds the state AFTER frame t was simulated and the input the ghost applied ON frame t, so a
(state, action) training pair is (data[t-1] state columns, data[t] input columns) within one ghost.
`flags` is a bitfield; bit meanings are listed in FLAG_BITS below.

    python tools/dump_ghosts.py samples/tracks/luigi-circuit/ghosts/lc-top100-pb --out samples/data/dumps/lc-top100-pb.npz
    python tools/dump_ghosts.py samples/lc-rta-1-08-733.rkg --out /tmp/wr.npz --keep-csv
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from prepare_ghosts import parse_rkg  # noqa: E402

FLAG_BITS = [
    "accelerate", "brake", "touching_ground", "hop", "drift", "boost", "mushroom_boost", "wheelie",
    "trick", "airtime_over_20", "wall_collision", "jump_pad", "zipper", "cannon", "respawn",
    "burnout", "hit", "ssmt_charged",
]
RACE_STAGE = 2


def find_ghosts(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            out += [os.path.join(p, f) for f in sorted(os.listdir(p)) if f.lower().endswith(".rkg")]
        else:
            out.append(p)
    return [os.path.abspath(p) for p in out]


def replay(binary, rkg, csv_path, timeout):
    """Run one replay in its own working directory (results.txt is written to the cwd)."""
    work = tempfile.mkdtemp(prefix="kdump_")
    env = dict(os.environ, KINOKO_FILESYSTEM_ROOT=ROOT)
    try:
        proc = subprocess.run([binary, "replay", "--ghost", rkg, "--dump", csv_path],
                capture_output=True, text=True, timeout=timeout, cwd=work, env=env)
        results = os.path.join(work, "results.txt")
        reason = open(results).read().strip().replace("\n", " | ") if os.path.exists(results) else ""
        if proc.returncode != 0 and not reason:
            reason = (proc.stderr or proc.stdout).strip().replace("\n", " | ")[-300:]
        return proc.returncode == 0, reason
    except subprocess.TimeoutExpired:
        return False, "timeout"
    finally:
        shutil.rmtree(work, ignore_errors=True)


def load_csv(path):
    with open(path, newline="") as f:
        reader = csv.reader(f)
        columns = next(reader)
        rows = np.array([[float(v) for v in r] for r in reader], dtype=np.float64)
    return columns, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help=".rkg files and/or folders of them")
    ap.add_argument("--out", required=True, help="output .npz")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build-host-release", "kinoko_host.exe"))
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--keep-csv", action="store_true", help="keep the per-ghost CSVs next to the .npz")
    args = ap.parse_args()

    ghosts = find_ghosts(args.inputs)
    if not ghosts:
        sys.exit("no .rkg files found")
    if not os.path.exists(args.binary):
        sys.exit(f"{args.binary} not found; build it: cd build-host-release && ninja kinoko_host")

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    csv_dir = os.path.join(out_dir, os.path.splitext(os.path.basename(args.out))[0] + "_csv")
    os.makedirs(csv_dir, exist_ok=True)

    t0 = time.time()

    def job(rkg):
        name = os.path.splitext(os.path.basename(rkg))[0]
        header = parse_rkg(open(rkg, "rb").read())
        csv_path = os.path.join(csv_dir, name + ".csv")
        ok, reason = replay(args.binary, rkg, csv_path, args.timeout)
        return name, header, csv_path, ok, reason

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        results = list(pool.map(job, ghosts))

    columns = None
    chunks, idx, frames, names, meta = [], [], [], [], []
    failed = []
    for name, header, csv_path, ok, reason in results:
        if not ok or not os.path.exists(csv_path):
            failed.append((name, reason))
            print(f"FAIL  {name}: {reason or 'no output'}")
            continue
        cols, rows = load_csv(csv_path)
        columns = columns or cols
        assert cols == columns, "column layout changed between ghosts"
        stage = rows[:, columns.index("stage")]
        race = rows[stage >= RACE_STAGE]
        gi = len(names)
        names.append(name)
        chunks.append(race.astype(np.float32))
        idx.append(np.full(len(race), gi, dtype=np.int32))
        frames.append(np.arange(len(race), dtype=np.int32))
        meta.append(dict(name=name, course=header["course"], vehicle=header["vehicle"],
                         character=header["character"], headerTimeMs=header["timeMs"], frames=len(race)))
        print(f"ok    {name}: {len(race)} race frames")

    if not names:
        sys.exit("no ghost replayed successfully")

    np.savez_compressed(args.out, data=np.concatenate(chunks), columns=np.array(columns),
            ghost_idx=np.concatenate(idx), race_frame=np.concatenate(frames),
            names=np.array(names), meta=json.dumps(meta), flag_bits=np.array(FLAG_BITS))
    if not args.keep_csv:
        shutil.rmtree(csv_dir, ignore_errors=True)

    print(f"\n{len(names)}/{len(ghosts)} ghosts replayed OK, {sum(len(c) for c in chunks)} rows "
          f"-> {args.out} ({os.path.getsize(args.out) / 1e6:.1f} MB) in {time.time() - t0:.1f}s")
    if failed:
        print(f"{len(failed)} failed: " + ", ".join(n for n, _ in failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
