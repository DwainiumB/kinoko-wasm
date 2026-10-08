#!/usr/bin/env python3
"""Train a policy that imitates ghost inputs from a tools/dump_ghosts.py dataset.

    python tools/train_policy.py samples/data/dumps/lc-top100-pb.npz --out samples/data/dumps/lc_policy.npz

The split is by ghost (whole runs are held out), never by frame, because neighbouring frames of one
run are nearly identical and a frame split would leak. Reported next to the model are two trivial
baselines on the held-out ghosts: always predicting the most common value, and repeating the input of
the previous frame. The model is only useful where it beats the latter.
"""

from __future__ import annotations

import argparse
import json
import time

import numpy as np

from policy_net import Policy, Targets, make_pairs


def adam_step(p, g, m, v, t, lr, b1=0.9, b2=0.999, eps=1e-8):
    for k in p:
        m[k] = b1 * m[k] + (1 - b1) * g[k]
        v[k] = b2 * v[k] + (1 - b2) * g[k] * g[k]
        p[k] -= lr * (m[k] / (1 - b1 ** t)) / (np.sqrt(v[k] / (1 - b2 ** t)) + eps)


def train_epochs(pol, xtr_norm, ytr, epochs, lr, batch_size, rng, log=print, label=""):
    """Run Adam for `epochs` passes over (already normalized) features. Returns the mean loss of the last
    epoch. Used by main() and by dagger.py, which calls it repeatedly to fine-tune the same policy."""
    m = {k: np.zeros_like(v) for k, v in pol.p.items()}
    v = {k: np.zeros_like(v) for k, v in pol.p.items()}
    n, step, mean_loss = len(xtr_norm), 0, 0.0
    for epoch in range(1, epochs + 1):
        order = rng.permutation(n)
        cur_lr = lr * 0.5 * (1 + np.cos(np.pi * (epoch - 1) / epochs))
        tot = 0.0
        for i in range(0, n, batch_size):
            b = order[i:i + batch_size]
            loss, g = pol.loss_and_grads(xtr_norm[b], {k: val[b] for k, val in ytr.items()})
            step += 1
            adam_step(pol.p, g, m, v, step, cur_lr)
            tot += loss * len(b)
        mean_loss = tot / n
        if log:
            log(f"{label}epoch {epoch:3d}  train {mean_loss:.4f}")
    return mean_loss


def report(title, pol, x, y, prev_y, majority):
    pr = pol.predict_proba(x)
    print(f"\n{title}")
    print(f"  {'head':<10}{'model':>9}{'repeat-prev':>13}{'majority':>10}")
    for key in ("stickX", "stickY", "trick"):
        acc = (pr[key].argmax(1) == y[key]).mean()
        print(f"  {key:<10}{acc:>9.3f}{(prev_y[key] == y[key]).mean():>13.3f}{(y[key] == majority[key]).mean():>10.3f}")
    for j, b in enumerate(pol.targets.bit_ids):
        acc = ((pr["bits"][:, j] > 0.5) == (y["bits"][:, j] > 0.5)).mean()
        print(f"  {'button b' + str(b):<10}{acc:>9.3f}{(prev_y['bits'][:, j] == y['bits'][:, j]).mean():>13.3f}"
              f"{(y['bits'][:, j] == majority['bits'][j]).mean():>10.3f}")
    # How far off the stick is when it is wrong, in stick-value units (steps are 1/7).
    t = pol.targets
    err = np.abs(t.stick_x_vals[pr["stickX"].argmax(1)] - t.stick_x_vals[y["stickX"]])
    print(f"  mean |stickX error| = {err.mean():.4f}   (repeat-prev: "
          f"{np.abs(t.stick_x_vals[prev_y['stickX']] - t.stick_x_vals[y['stickX']]).mean():.4f})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dataset", help=".npz from dump_ghosts.py")
    ap.add_argument("--out", required=True, help="where to save the trained policy (.npz)")
    ap.add_argument("--no-prev-input", action="store_true",
            help="do not feed the previous frame's input as a feature (harder; closer to what a search needs)")
    ap.add_argument("--hidden", type=int, default=256)
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch-size", type=int, default=2048)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--val-ghosts", type=int, default=10, help="number of whole ghosts held out for validation")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    d = np.load(args.dataset)
    cols = [str(c) for c in d["columns"]]
    use_prev = not args.no_prev_input
    x, tgt, gid, prev, names = make_pairs(d["data"], cols, d["ghost_idx"], use_prev)
    print(f"{len(x)} pairs, {x.shape[1]} features, {len(np.unique(gid))} ghosts, prev-input feature: {use_prev}")

    rng = np.random.default_rng(args.seed)
    ghosts = rng.permutation(np.unique(gid))
    val_set = set(ghosts[:args.val_ghosts].tolist())
    is_val = np.array([g in val_set for g in gid])
    tr, va = ~is_val, is_val
    print(f"train: {tr.sum()} rows / {len(ghosts) - len(val_set)} ghosts   val: {va.sum()} rows / {len(val_set)} ghosts")

    targets = Targets.fit(tgt[tr], cols)
    y_all, prev_all = targets.encode(tgt, cols), targets.encode(prev, cols)
    sl = lambda y, m: {k: v[m] for k, v in y.items()}
    ytr, yva, pva = sl(y_all, tr), sl(y_all, va), sl(prev_all, va)
    majority = {k: np.bincount(ytr[k]).argmax() for k in ("stickX", "stickY", "trick")}
    majority["bits"] = (ytr["bits"].mean(0) > 0.5).astype(np.float32)
    print(f"stick values: {len(targets.stick_x_vals)}  trick ids: {targets.trick_vals.tolist()}  button bits: {targets.bit_ids}")

    pol = Policy(x.shape[1], targets, hidden=args.hidden, seed=args.seed)
    pol.fit_normalizer(x[tr])
    xtr, xva = (x[tr] - pol.mean) / pol.std, x[va]

    m = {k: np.zeros_like(v) for k, v in pol.p.items()}
    v = {k: np.zeros_like(v) for k, v in pol.p.items()}
    n, step, best, best_p = len(xtr), 0, np.inf, None
    t0 = time.time()
    for epoch in range(1, args.epochs + 1):
        order = rng.permutation(n)
        lr = args.lr * 0.5 * (1 + np.cos(np.pi * (epoch - 1) / args.epochs))  # cosine decay
        tot = 0.0
        for i in range(0, n, args.batch_size):
            b = order[i:i + args.batch_size]
            loss, g = pol.loss_and_grads(xtr[b], {k: val[b] for k, val in ytr.items()})
            step += 1
            adam_step(pol.p, g, m, v, step, lr)
            tot += loss * len(b)
        vl = pol.eval_loss(xva, yva)
        if vl < best:
            best, best_p = vl, {k: val.copy() for k, val in pol.p.items()}
        print(f"epoch {epoch:3d}  train {tot / n:.4f}  val {vl:.4f}{'  *' if vl == best else ''}  ({time.time() - t0:.0f}s)")
    pol.p = best_p

    report("held-out ghosts", pol, xva, yva, pva, majority)
    pol.save(args.out, names, use_prev)
    print(f"\nsaved {args.out}")


if __name__ == "__main__":
    main()
