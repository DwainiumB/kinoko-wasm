#!/usr/bin/env python3
"""Rolling-best-runs loop: search, collect the fastest runs, retrain the policy on them, search again.

    round r:  beam search (seeded with the fastest runs so far, policy_r as the prior)
              -> the fastest distinct finished runs it found join a pool; the pool keeps the best --pool-size
              -> replay the pool in Kinoko to get per-frame (state, input) rows
              -> fine-tune the policy on the original ghosts plus the pool            -> policy_{r+1}

`--arm fixed` runs exactly the same loop (same seeds, same pool growth) but never updates the policy. Running
both arms side by side separates the effect of retraining from the effect of simply searching longer.

    python tools/evolve.py --arm retrain --out-dir samples/tracks/luigi-circuit/runs/evolve/retrain
    python tools/evolve.py --arm fixed   --out-dir samples/tracks/luigi-circuit/runs/evolve/fixed
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import multiprocessing as mp
import os
import re
import shutil
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from brute_force import GO_FRAME, decode_rkg  # noqa: E402
from drive_policy import drive  # noqa: E402
from kinoko_search import DEFAULT_BINARY, KinokoSearch  # noqa: E402
from policy_net import Policy, make_pairs  # noqa: E402
from prepare_ghosts import parse_rkg  # noqa: E402
from train_policy import train_epochs  # noqa: E402


def replay_rows(path):
    """Replay one .rkg in a fresh engine; returns (columns, race rows from the GO frame on)."""
    frames = decode_rkg(path)[3]
    rows = []
    with KinokoSearch() as race:
        for f in frames:
            row = race.step(f[0], f[2], f[3], f[1])
            rows.append(row)
            if race.end is not None:
                break
        cols = race.cols
    rows = np.array(rows)
    return cols, rows[rows[:, cols.index("stage")] >= 2]


def load_base_pairs(path, pol, fraction, chunk_ghosts=40, seed=1234):
    """(features, encoded targets, feature names) for the base dataset, built a chunk of ghosts at a time with a
    random `fraction` of the frame pairs kept, so memory never holds the full dataset's pairs."""
    d = np.load(path)
    cols = [str(c) for c in d["columns"]]
    data, gidx = d["data"], d["ghost_idx"]
    bounds = np.flatnonzero(np.diff(gidx)) + 1
    starts = np.concatenate([[0], bounds])
    ends = np.concatenate([bounds, [len(gidx)]])
    rng = np.random.default_rng(seed)
    xs, ys, names = [], [], None
    for i in range(0, len(starts), chunk_ghosts):
        a, b = starts[i], ends[min(i + chunk_ghosts, len(starts)) - 1]
        x, tgt, _g, _p, names = make_pairs(data[a:b], cols, gidx[a:b], False)
        y = pol.targets.encode(tgt, cols)
        keep = np.sort(rng.choice(len(x), int(len(x) * fraction), replace=False)) if fraction < 1.0 else slice(None)
        xs.append(x[keep])
        ys.append({k: v[keep] for k, v in y.items()})
        del x, tgt, y, _p
    return np.concatenate(xs), {k: np.concatenate([y[k] for y in ys]) for k in ys[0]}, names, cols


def file_key(path):
    return hashlib.sha1(open(path, "rb").read()).hexdigest()


def run_info(path):
    return parse_rkg(open(path, "rb").read())["timeMs"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arm", choices=("retrain", "fixed"), required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--ghosts", default=os.path.join(ROOT, "samples", "tracks", "luigi-circuit", "ghosts", "lc-top100-pb"))
    ap.add_argument("--ghost-offset", type=int, default=97, help="initial seeds: skip this many of the fastest ghosts")
    ap.add_argument("--base-dataset", default=os.path.join(ROOT, "samples", "data", "dumps", "lc-top100-pb.npz"))
    ap.add_argument("--policy", default=os.path.join(ROOT, "samples", "data", "dumps", "lc_policy_path.npz"))
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--seeds", type=int, default=3, help="protected ghost lines per round")
    ap.add_argument("--pool-size", type=int, default=60)
    ap.add_argument("--top-k", type=int, default=20, help="finished runs taken from each search")
    ap.add_argument("--beam", type=int, default=16)
    ap.add_argument("--workers", type=int, default=10)
    ap.add_argument("--epochs", type=int, default=5)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--restart", action="store_true",
            help="retrain from the original policy weights every round instead of continuing from the last round")
    ap.add_argument("--max-repeat", type=int, default=8, help="cap on how many times the pool rows are repeated in training")
    ap.add_argument("--base-fraction", type=float, default=1.0,
            help="random fraction of the base dataset's frame pairs used when retraining (saves memory and time)")
    ap.add_argument("--seed-base", type=int, default=0, help="added to the round number to seed each search")
    ap.add_argument("--search-script", default="beam_search.py", help="search script in tools/ to run each round")
    ap.add_argument("--search-args", default="", help="extra arguments passed to the search script, e.g. '--lookahead 30'")
    ap.add_argument("--init-pool", nargs="+",
            help="folders of earlier evolve runs; their found/best runs start the pool instead of the slow ghosts")
    ap.add_argument("--target-ms", type=int, default=0, help="stop as soon as the pool's best finish is at or below this")
    ap.add_argument("--patience", type=int, default=0, help="stop after this many rounds without a new best (0 = never)")
    args = ap.parse_args()

    for var in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"):
        pass  # the main process keeps multi-threaded BLAS for training; search workers set their own
    os.makedirs(args.out_dir, exist_ok=True)
    log = open(os.path.join(args.out_dir, "log.jsonl"), "a")

    # Initial seeds: the slowest of the leaderboard ghosts, where there is a gap to close.
    files = sorted(f for f in os.listdir(args.ghosts) if f.lower().endswith(".rkg"))[args.ghost_offset:]
    initial = [os.path.join(args.ghosts, f) for f in files[:args.seeds]]
    pool = {}  # key -> (time_ms, path)
    for p in initial:
        pool[file_key(p)] = (run_info(p), p)
    if args.init_pool:
        pool = {}
        for folder in args.init_pool:
            for pat in ("round_*/found/*.rkg", "round_*/best.rkg"):
                for p in glob.glob(os.path.join(folder, pat)):
                    pool[file_key(p)] = (run_info(p), p)
        for k, _ in sorted(pool.items(), key=lambda kv: kv[1])[args.pool_size:]:
            del pool[k]
        print(f"pool started from {len(pool)} earlier runs, best {min(t for t, _ in pool.values()) / 1000:.3f}s", flush=True)

    policy_path = args.policy
    pol = Policy.load(policy_path)
    if args.arm == "retrain":
        bx, by, names, cols = load_base_pairs(args.base_dataset, pol, args.base_fraction)
        print(f"base training pairs: {len(bx)} (fraction {args.base_fraction})", flush=True)
    else:
        cols = [str(c) for c in np.load(args.base_dataset)["columns"]]
    rows_cache = {}

    print(f"arm {args.arm}: initial seeds " + ", ".join(f"{pool[k][0] / 1000:.3f}s" for k in pool), flush=True)
    t_start = time.time()
    best_so_far, stale = min(t for t, _ in pool.values()), 0
    for r in range(args.rounds):
        t0 = time.time()
        rdir = os.path.join(args.out_dir, f"round_{r}")
        seeds_dir = os.path.join(rdir, "seeds")
        shutil.rmtree(rdir, ignore_errors=True)
        os.makedirs(seeds_dir)
        best_pool = sorted(pool.values())[:args.seeds]
        for i, (t_ms, p) in enumerate(best_pool):
            shutil.copy(p, os.path.join(seeds_dir, f"seed-{t_ms:06d}-{i}.rkg"))

        cmd = [sys.executable, "-u", os.path.join(HERE, args.search_script), "--ghosts", seeds_dir,
               "--top-ghosts", str(args.seeds), "--policy", policy_path, "--out", os.path.join(rdir, "best.rkg"),
               "--save-top", str(args.top_k), "--top-dir", os.path.join(rdir, "found"), "--beam", str(args.beam),
               "--workers", str(args.workers), "--seed", str(r + args.seed_base)] + args.search_args.split()
        # The search writes straight into search.log so a round can be followed live (tail -f / Get-Content -Wait).
        log_path = os.path.join(rdir, "search.log")
        with open(log_path, "w") as lf:
            subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, cwd=ROOT)
        search_text = open(log_path).read()
        m = re.search(r"best finish: ([\d.]+)s", search_text)
        if not m:
            print("search failed, see", os.path.join(rdir, "search.log"), flush=True)
            sys.exit(1)
        best_s = float(m.group(1))
        comp = re.search(r"winning run by origin: (.*)", search_text)
        seed_times = [t for t, _ in best_pool]

        # Add the new runs, keep the fastest distinct ones.
        found_dir = os.path.join(rdir, "found")
        new = 0
        for f in sorted(os.listdir(found_dir)) if os.path.isdir(found_dir) else []:
            p = os.path.join(found_dir, f)
            k = file_key(p)
            if k not in pool:
                pool[k] = (run_info(p), p)
                new += 1
        for k, _ in sorted(pool.items(), key=lambda kv: kv[1])[args.pool_size:]:
            del pool[k]
        times = sorted(t for t, _ in pool.values())

        entry = dict(arm=args.arm, round=r, seed_times_ms=seed_times, best_ms=int(round(best_s * 1000)),
                winning_run_origin=comp.group(1) if comp else "", new_runs=new, pool=len(pool), pool_best_ms=times[0],
                pool_median_ms=times[len(times) // 2])

        if args.arm == "retrain":
            # Per-frame data for every run in the pool (cached by file hash), in parallel.
            todo = [(k, p) for k, (_, p) in pool.items() if k not in rows_cache]
            for var in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"):
                os.environ[var] = "1"
            with mp.get_context("spawn").Pool(min(8, max(1, len(todo)))) as mpool:
                for (k, _), (_c, rows) in zip(todo, mpool.map(replay_rows, [p for _, p in todo])):
                    rows_cache[k] = rows
            pool_rows = [rows_cache[k] for k in pool]
            data = np.concatenate(pool_rows)
            gidx = np.concatenate([np.full(len(x), i, dtype=np.int32) for i, x in enumerate(pool_rows)])
            px, ptgt, _pg, _pp, _n = make_pairs(data, cols, gidx, False)
            py = pol.targets.encode(ptgt, cols)
            rep = int(np.clip(round(len(bx) / len(px)), 1, args.max_repeat))
            if args.restart:
                pol = Policy.load(args.policy)
            x_all = np.concatenate([bx] + [px] * rep)
            y_all = {k: np.concatenate([by[k]] + [py[k]] * rep) for k in by}
            loss = train_epochs(pol, (x_all - pol.mean) / pol.std, y_all, args.epochs, args.lr, 2048,
                    np.random.default_rng(r + args.seed_base), log=None)
            policy_path = os.path.join(args.out_dir, f"policy_{r + 1}.npz")
            pol.save(policy_path, names, False)
            res = drive(pol, DEFAULT_BINARY, max_frames=GO_FRAME + 4700)
            entry.update(train_rows=len(x_all), pool_rows=len(px), pool_repeat=rep, train_loss=round(float(loss), 4),
                    standalone=("finished %.3fs" % (res.time_ms / 1000) if res.finished else "%.2f/3 laps" % max(res.completion - 1, 0)))

        entry["minutes"] = round((time.time() - t0) / 60, 1)
        log.write(json.dumps(entry) + "\n")
        log.flush()
        print(json.dumps(entry), flush=True)

        if args.target_ms and times[0] <= args.target_ms:
            print(f"TARGET REACHED: {times[0] / 1000:.3f}s <= {args.target_ms / 1000:.3f}s", flush=True)
            break
        if times[0] < best_so_far:
            best_so_far, stale = times[0], 0
        else:
            stale += 1
        if args.patience and stale >= args.patience:
            print(f"STOPPING: no new best for {stale} rounds (best {times[0] / 1000:.3f}s)", flush=True)
            break

    print(f"done in {(time.time() - t_start) / 60:.0f} min; best pool time {times[0] / 1000:.3f}s", flush=True)


if __name__ == "__main__":
    main()
