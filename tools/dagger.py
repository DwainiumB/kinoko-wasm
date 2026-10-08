#!/usr/bin/env python3
"""DAgger: teach the ghost-imitation policy to recover from its own mistakes.

Behavior cloning only sees the states ghosts visit, so after its first small error the policy is in a
state it never trained on and the errors compound (see drive_policy.py: it stalls and wanders). DAgger
fixes the data instead of the model:

    repeat:
        roll the current policy out in Kinoko (sampled, so every rollout differs)   -> visited states
        label every visited state with an expert                                    -> (state, action)
        add those pairs to the ghost data and fine-tune the policy

The expert (ghost-based, no extra information is invented):
  * if the state is close to something a ghost did (position, heading and speed, nearest neighbours
    among all ghost frames), it votes among the k nearest ghost frames' inputs;
  * otherwise (kart stuck, spun out, off the racing line) it falls back to a pure-pursuit controller:
    steer toward the road 3 checkpoints ahead, hold accelerate.

    python tools/dagger.py --dataset samples/data/dumps/lc-top100-pb.npz --policy samples/data/dumps/lc_policy_path.npz \
        --out-dir samples/data/dagger --iters 10

Each iteration prints the deterministic closed-loop result of the policy it started from, which is the
number that matters (laps completed / finish time), not the training loss.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from brute_force import GO_FRAME, find_best_start_boost_frame  # noqa: E402
from drive_policy import DEFAULT_BINARY, KinokoRace, snap_stick  # noqa: E402
from policy_net import Policy, _rotate, make_pairs, state_features  # noqa: E402
from train_policy import train_epochs  # noqa: E402

HEADING_SCALE = 200.0  # world units per unit of heading-vector difference
SPEED_SCALE = 2.0      # world units per unit of speed
PURSUIT_K = 3          # look-ahead checkpoint used by the fallback controller
PURSUIT_GAIN = 1.8


def state_keys(rows: np.ndarray, cols: list[str]) -> np.ndarray:
    """Low-dimensional description of a state used to find similar ghost frames."""
    c = lambda n: rows[:, cols.index(n)].astype(np.float64)
    q = np.stack([c("qx"), c("qy"), c("qz"), c("qw")], axis=1)
    fwd = _rotate(q, np.tile(np.array([0.0, 0.0, 1.0]), (len(rows), 1)))[:, [0, 2]]
    fwd /= np.maximum(np.linalg.norm(fwd, axis=1, keepdims=True), 1e-6)
    return np.column_stack([c("px"), c("pz"), fwd * HEADING_SCALE, c("speed") * SPEED_SCALE])


def nearest_idx(vals: np.ndarray, v: np.ndarray) -> np.ndarray:
    return np.abs(v[:, None] - vals[None, :]).argmin(axis=1)


class Expert:
    def __init__(self, data, cols, ghost_idx, targets, k=8, near=800.0, far_mode="drop"):
        _x, tgt, _g, prev, _names = make_pairs(data, cols, ghost_idx, False)
        self.cols, self.targets, self.k, self.near, self.far_mode = cols, targets, k, near, far_mode
        self.tree = cKDTree(state_keys(prev, cols))
        self.y = targets.encode(tgt, cols)

    def label(self, rows: np.ndarray, feats: np.ndarray, names: list[str]):
        """Expert inputs for each state row. Returns (encoded targets dict, keep mask).

        States farther than `near` from every ghost frame are either dropped (far_mode "drop": the ghosts say
        nothing useful there) or labelled by the pursuit controller (far_mode "pursuit")."""
        t, n = self.targets, len(rows)
        dist, idx = self.tree.query(state_keys(rows, self.cols), k=self.k)
        out = {}
        for key, ncls in (("stickX", len(t.stick_x_vals)), ("stickY", len(t.stick_y_vals)), ("trick", len(t.trick_vals))):
            votes = np.zeros((n, ncls))
            lab = self.y[key][idx]
            for j in range(self.k):
                np.add.at(votes, (np.arange(n), lab[:, j]), 1.0)
            out[key] = votes.argmax(axis=1)
        out["bits"] = (self.y["bits"][idx].mean(axis=1) > 0.5).astype(np.float32)

        far = dist[:, 0] > self.near
        if far.any() and self.far_mode == "pursuit":
            bearing = feats[far, names.index(f"pathMid{PURSUIT_K}2")]
            steer = np.clip(-PURSUIT_GAIN * bearing, -1.0, 1.0)  # +bearing is toward negative stick X
            out["stickX"][far] = nearest_idx(t.stick_x_vals, steer)
            out["stickY"][far] = nearest_idx(t.stick_y_vals, np.zeros(far.sum()))
            out["trick"][far] = nearest_idx(t.trick_vals.astype(np.float64), np.zeros(far.sum()))
            bits = np.zeros((far.sum(), len(t.bit_ids)), dtype=np.float32)
            if 0 in t.bit_ids:
                bits[:, t.bit_ids.index(0)] = 1.0  # accelerate
            out["bits"][far] = bits
        keep = np.ones(n, dtype=bool) if self.far_mode == "pursuit" else ~far
        return out, keep, float((~far).mean())


def rollout(job):
    """One race with the policy in `path`. Runs in a worker process.

    Returns the visited state rows (from just before GO onward) and the outcome."""
    path, seed, temperature, max_race_frames, stall_frames, start_frame, binary = job
    pol = Policy.load(path)
    rng = np.random.default_rng(seed) if temperature > 0 else None
    rows, best_comp, since_best, row = [], 0.0, 0, None
    with KinokoRace(binary, max_frames=GO_FRAME + max_race_frames + 50) as race:
        cols = race.cols
        ci, li = cols.index("completion"), cols.index("lap")
        frame = 0
        while True:
            if frame < GO_FRAME:
                inp = (1 if frame >= start_frame else 0, 0, 0.0, 0.0)
            else:
                f, _ = state_features(row[None, :], cols, pol.use_prev_input)
                a = pol.act(f, rng, temperature)
                inp = (int(a["buttons"][0]), int(a["trick"][0]), snap_stick(a["stickX"][0]), snap_stick(a["stickY"][0]))
            row = race.step(inp[0], inp[2], inp[3], inp[1])
            if row is None:
                break
            frame += 1
            if frame >= GO_FRAME - 1:
                rows.append(row)
                comp = row[ci]
                if comp > best_comp + 1e-4:
                    best_comp, since_best = comp, 0
                else:
                    since_best += 1
                if since_best > stall_frames or frame - GO_FRAME > max_race_frames:
                    break
        end = race.end
    finished = bool(end and end["finished"])
    return dict(rows=np.array(rows), cols=cols, finished=finished, time_ms=end["timeMs"] if finished else -1,
            laps=max(best_comp - 1.0, 0.0), frames=frame, seed=seed)


def score(res):
    """Sort key: finished runs by time, otherwise by distance covered."""
    return (1, -res["time_ms"]) if res["finished"] else (0, res["laps"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dataset", required=True)
    ap.add_argument("--policy", required=True, help="starting policy (.npz from train_policy.py --no-prev-input)")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--iters", type=int, default=10)
    ap.add_argument("--rollouts", type=int, default=8, help="stochastic rollouts per iteration")
    ap.add_argument("--temperature", type=float, default=1.0)
    ap.add_argument("--epochs", type=int, default=6)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--batch-size", type=int, default=2048)
    ap.add_argument("--dagger-weight", type=int, default=3, help="repeat each relabeled row this many times")
    ap.add_argument("--max-race-frames", type=int, default=4600)
    ap.add_argument("--stall-frames", type=int, default=400, help="cut a rollout after this many frames without progress")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--near", type=float, default=800.0, help="max distance to a ghost state for the ghost expert to be used")
    ap.add_argument("--far-mode", choices=("drop", "pursuit"), default="drop",
            help="what to do with states farther than --near from every ghost frame")
    ap.add_argument("--binary", default=DEFAULT_BINARY)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    d = np.load(args.dataset)
    cols = [str(c) for c in d["columns"]]
    pol = Policy.load(args.policy)
    assert not pol.use_prev_input, "DAgger needs a policy trained with --no-prev-input"

    gx, _gt, _gid, _prev, names = make_pairs(d["data"], cols, d["ghost_idx"], False)
    expert = Expert(d["data"], cols, d["ghost_idx"], pol.targets, near=args.near, far_mode=args.far_mode)
    gy = expert.y
    start_frame = find_best_start_boost_frame()[0]
    rng = np.random.default_rng(args.seed)
    print(f"{len(gx)} ghost pairs, expert tree ready, start-boost frame {start_frame}", flush=True)

    dag_x, dag_y = [], []
    best, best_path = None, os.path.join(args.out_dir, "best.npz")
    log = open(os.path.join(args.out_dir, "log.jsonl"), "a")
    # Workers inherit these when they start. Without them every worker's BLAS reserves memory for one thread
    # per core (about 1.6 GB each on this machine) and a dozen workers can exhaust the paging file.
    for var in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"):
        os.environ[var] = "1"
    pool = ProcessPoolExecutor(max_workers=args.workers)

    def run_iteration_rollouts(path, it):
        jobs = [(path, 0, 0.0, args.max_race_frames, 10_000, start_frame, args.binary)]  # deterministic eval, no early cut
        jobs += [(path, int(rng.integers(1 << 30)), args.temperature, args.max_race_frames, args.stall_frames,
                  start_frame, args.binary) for _ in range(args.rollouts)]
        return list(pool.map(rollout, jobs))

    for it in range(1, args.iters + 2):
        t0 = time.time()
        cur = os.path.join(args.out_dir, f"iter_{it - 1}.npz")
        pol.save(cur, names, False)
        results = run_iteration_rollouts(cur, it) if it <= args.iters else [rollout((cur, 0, 0.0, args.max_race_frames,
                10_000, start_frame, args.binary))]
        ev, rolls = results[0], results[1:]
        assert ev["cols"] == cols, "drive columns differ from dataset columns"

        entry = dict(iter=it - 1, eval_finished=ev["finished"], eval_time_ms=ev["time_ms"], eval_laps=round(ev["laps"], 3),
                dagger_rows=int(sum(len(x) for x in dag_x)))
        if best is None or score(ev) > score(best):
            best = ev
            pol.save(best_path, names, False)
            entry["best"] = True
        print(f"policy iter_{it - 1}: " + (f"FINISHED {ev['time_ms'] / 1000:.3f}s" if ev["finished"] else f"{ev['laps']:.2f}/3 laps")
              + (" *best" if entry.get("best") else ""), flush=True)
        log.write(json.dumps(entry) + "\n")
        log.flush()
        if it > args.iters:
            break

        # Relabel the states the stochastic rollouts visited.
        new_x, new_y, near_frac = [], [], []
        for r in rolls:
            if len(r["rows"]) < 2:
                continue
            f, _ = state_features(r["rows"], cols, False)
            y, keep, nf = expert.label(r["rows"], f, names)
            new_x.append(f[keep])
            new_y.append({k: v[keep] for k, v in y.items()})
            near_frac.append(nf)
        if new_x:
            dag_x.append(np.concatenate(new_x))
            dag_y.append({k: np.concatenate([y[k] for y in new_y]) for k in new_y[0]})
        roll_laps = [round(float(r["laps"]), 2) for r in rolls]
        print(f"  rollouts laps {roll_laps}  +{sum(len(x) for x in new_x)} relabeled rows "
              f"({100 * np.mean(near_frac):.0f}% of visited states within {args.near:.0f} of a ghost; "
              f"far ones {args.far_mode})   total dagger rows "
              f"{sum(len(x) for x in dag_x)}", flush=True)

        # Fine-tune on ghosts + (repeated) relabeled rows.
        dx = np.concatenate(dag_x)
        dy = {k: np.concatenate([y[k] for y in dag_y]) for k in dag_y[0]}
        rep = args.dagger_weight
        x_all = np.concatenate([gx] + [dx] * rep)
        y_all = {k: np.concatenate([gy[k]] + [dy[k]] * rep) for k in gy}
        loss = train_epochs(pol, (x_all - pol.mean) / pol.std, y_all, args.epochs, args.lr, args.batch_size, rng, log=None)
        print(f"  fine-tuned {args.epochs} epochs on {len(x_all)} rows, loss {loss:.4f}  ({time.time() - t0:.0f}s)", flush=True)

    np.savez_compressed(os.path.join(args.out_dir, "dagger_buffer.npz"), x=np.concatenate(dag_x),
            **{f"y_{k}": np.concatenate([y[k] for y in dag_y]) for k in dag_y[0]})
    print(f"\nbest: " + (f"finished in {best['time_ms'] / 1000:.3f}s" if best["finished"] else f"{best['laps']:.2f}/3 laps")
          + f"   saved {best_path}", flush=True)


if __name__ == "__main__":
    main()
