#!/usr/bin/env python3
"""Trains a surrogate model (see tools/surrogate.py) from an eval log produced by running
tools/brute_force.py or tools/brute_gui_server.py with SearchConfig.eval_log_path set (CLI:
--eval-log-path, GUI: not yet exposed -- pass it through a raw API call, or ask for the field to be
added if you want it in the GUI).

    python tools/train_surrogate.py --log samples/data/eval_log.jsonl \
        --course 8 --character 22 --vehicle 32 --n-points 412 --step 1 \
        --search-accelerate --search-trick --out tools/surrogate_lc.npz

The resulting .npz can be pointed to from SearchConfig.surrogate_path (CLI: --surrogate-path) to
have the search cheaply pre-rank a larger pool of candidates per generation instead of real-
evaluating every one -- see brute_force.py's _run() and surrogate.py's module docstring for why.
"""

from __future__ import annotations

import argparse

from surrogate import SurrogateMeta, SurrogateModel, load_dataset


def main():
    parser = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", required=True, help="path to the JSONL eval log")
    parser.add_argument("--course", type=int, required=True)
    parser.add_argument("--character", type=int, required=True)
    parser.add_argument("--vehicle", type=int, required=True)
    parser.add_argument("--n-points", type=int, required=True)
    parser.add_argument("--step", type=int, required=True)
    parser.add_argument("--search-start-boost", action="store_true")
    parser.add_argument("--search-accelerate", action="store_true")
    parser.add_argument("--search-trick", action="store_true")
    parser.add_argument("--hidden-dim", type=int, default=64)
    parser.add_argument("--epochs", type=int, default=200)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--out", required=True, help="where to save the trained model (.npz)")
    args = parser.parse_args()

    meta = SurrogateMeta(args.course, args.character, args.vehicle, args.n_points, args.step,
            args.search_start_boost, args.search_accelerate, args.search_trick)

    print(f"loading {args.log} for {meta.to_dict()} ...")
    X, y = load_dataset(args.log, meta)
    print(f"{X.shape[0]} matching records, genome dim {X.shape[1]}, "
            f"progress range [{y.min():.1f}, {y.max():.1f}]")

    if X.shape[0] < 50:
        print(f"WARNING: only {X.shape[0]} training samples -- a surrogate this data-starved is "
                "unlikely to give useful predictions. Run more searches with --eval-log-path set "
                "to accumulate more data before relying on this model.")

    model = SurrogateModel(input_dim=X.shape[1], hidden_dim=args.hidden_dim, meta=meta)
    model.train(X, y, epochs=args.epochs, batch_size=args.batch_size, lr=args.lr, verbose=True)

    model.save(args.out)
    print(f"saved to {args.out}")


if __name__ == "__main__":
    main()
