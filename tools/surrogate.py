#!/usr/bin/env python3
"""A small, dependency-free (beyond numpy, already used elsewhere in this project) surrogate model
predicting a candidate genome's "progress" fitness score without running the real engine.

Why: the real evaluation (spawn kinoko_host, simulate hundreds of frames) costs ~80-150ms per
candidate even with parallel workers -- and improving mutations near a good seed have been measured
at roughly 1-in-thousands (see brute_force.py's fitness()/compute_progress_reference() docstrings
and this session's own testing). The bottleneck isn't finding a good mutation among the ones you
check, it's how many you can afford to check. A cheap (microseconds) surrogate lets the search
propose a much larger pool of candidates per generation, rank them by predicted quality, and spend
real engine time only on the most promising subset -- see brute_force.py's SearchConfig.surrogate_*
fields and _run()'s surrogate-filtering step for how this plugs in.

The surrogate predicts raw piecewise `progress` (see compute_progress_reference/piecewise_progress
in brute_force.py), NOT the full gated fitness score -- fitness()'s hard gate can produce a penalty
of 1e9 for a rejected candidate, which would dominate and wreck a regression loss. Progress alone is
a well-defined, bounded, always-computable quantity regardless of whether a candidate would pass the
gate, which is exactly what "is this candidate's raw trajectory quality promising" needs. The real
fitness() function -- gate, boost/completion/Z bonuses and all -- still makes the actual accept/
reject call on whichever candidates get real-evaluated; the surrogate only decides what's worth
checking in the first place.

A small hand-rolled MLP (numpy only, trained via Adam) is used instead of a heavier ML library
since numpy is already a dependency of nothing else here but is confirmed available, while
scikit-learn/torch are not -- this keeps the project's existing "stdlib + numpy, nothing heavier"
footprint for tools/*.py.
"""

from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

import numpy as np

# Guards log_evaluation()'s file write. brute_force.py's parallel search calls _evaluate() (which
# calls log_evaluation()) from multiple ThreadPoolExecutor worker threads at once -- without this,
# concurrent appends to the same file can interleave mid-write and corrupt lines (confirmed in
# practice: a real log ended up with a line that was just the truncated tail of a record, everything
# before it silently lost to an interleaved write from another thread).
_log_lock = threading.Lock()

# Cache of already-opened log file handles, keyed by path. Opening a file is cheap in general, but
# on this project's dev machine, any process that has imported numpy gets hit with ~200-250ms of
# per-open latency from Windows Defender's real-time scan of the write (confirmed by direct
# measurement: identical open+write code takes ~0.2ms before `import numpy` runs in the process and
# ~220ms after, with no other variable changed). Since numpy is a hard dependency of this module,
# every log_evaluation() call used to pay that cost -- at parallel_workers=8 that alone accounted for
# essentially all of the observed ~2000ms/iteration slowdown. Keeping one open handle per path for
# the process lifetime means the penalty is paid once (at first use) instead of once per evaluation.
_open_logs: dict[str, "object"] = {}


@dataclass
class SurrogateMeta:
    """Identifies which search configuration a surrogate/log record belongs to -- a genome vector's
    length and meaning depend entirely on these fields (n_points, which channels are present), so
    records/models must never be mixed across mismatched configs."""
    course: int
    character: int
    vehicle: int
    n_points: int
    step: int
    search_start_boost: bool
    search_accelerate: bool
    search_trick: bool

    def matches(self, other: "SurrogateMeta") -> bool:
        return (self.course, self.character, self.vehicle, self.n_points, self.step,
                self.search_start_boost, self.search_accelerate, self.search_trick) == \
               (other.course, other.character, other.vehicle, other.n_points, other.step,
                other.search_start_boost, other.search_accelerate, other.search_trick)

    def to_dict(self) -> dict:
        return {
            "course": self.course, "character": self.character, "vehicle": self.vehicle,
            "n_points": self.n_points, "step": self.step,
            "search_start_boost": self.search_start_boost,
            "search_accelerate": self.search_accelerate, "search_trick": self.search_trick,
        }

    @staticmethod
    def from_dict(d: dict) -> "SurrogateMeta":
        return SurrogateMeta(d["course"], d["character"], d["vehicle"], d["n_points"], d["step"],
                d["search_start_boost"], d["search_accelerate"], d["search_trick"])


def log_evaluation(log_path: str, genome: list, progress: Optional[float], completion: int,
        boost_charge: Optional[float], meta: SurrogateMeta) -> None:
    """Appends one JSONL record. Called once per real engine evaluation when
    SearchConfig.eval_log_path is set -- see brute_force.py's _run()."""
    record = {
        "genome": genome, "progress": progress, "completion": completion,
        "boostCharge": boost_charge, "timestamp": time.time(),
        **meta.to_dict(),
    }
    line = json.dumps(record) + "\n"
    with _log_lock:
        f = _open_logs.get(log_path)
        if f is None or f.closed:
            f = open(log_path, "a")
            _open_logs[log_path] = f
        f.write(line)
        f.flush()


def load_dataset(log_path: str, meta: SurrogateMeta):
    """Reads a JSONL log and returns (X, y) numpy arrays for every record matching `meta` and
    having a non-null progress value (progress is only computed for fitness_metric == "progress"
    runs -- see brute_force.py's fitness()). Records from a different course/character/vehicle/
    genome-shape are silently skipped, not errored, so one log file can accumulate data across many
    different search configurations over time."""
    xs, ys = [], []
    skipped_mismatch = 0
    skipped_no_progress = 0
    with open(log_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            if record.get("progress") is None:
                skipped_no_progress += 1
                continue
            record_meta = SurrogateMeta.from_dict(record)
            if not record_meta.matches(meta):
                skipped_mismatch += 1
                continue
            xs.append(record["genome"])
            ys.append(record["progress"])
    if not xs:
        raise ValueError(
                f"no matching records in {log_path} for {meta.to_dict()} "
                f"({skipped_mismatch} skipped for config mismatch, "
                f"{skipped_no_progress} skipped for missing progress)")
    return np.array(xs, dtype=np.float64), np.array(ys, dtype=np.float64)


class SurrogateModel:
    """A single-hidden-layer MLP (genome -> predicted progress), trained with Adam. Deliberately
    small (one hidden layer, modest width) since training sets from this project's scale (hundreds
    to low thousands of real evaluations) are small relative to genome dimensionality (400-1200+
    genes depending on config) -- a bigger network would just overfit faster, not generalize
    better, on data this size."""

    def __init__(self, input_dim: int, hidden_dim: int = 64, meta: Optional[SurrogateMeta] = None):
        self.meta = meta
        rng = np.random.default_rng(0)
        # He initialization -- matches the ReLU hidden layer.
        self.w1 = rng.normal(0, np.sqrt(2.0 / input_dim), size=(input_dim, hidden_dim))
        self.b1 = np.zeros(hidden_dim)
        self.w2 = rng.normal(0, np.sqrt(2.0 / hidden_dim), size=(hidden_dim, 1))
        self.b2 = np.zeros(1)
        # Input/output standardization -- fit during train(), applied in predict() too. Genome
        # entries are already roughly unit-scale (steering in [-1,1], accel/trick channels close
        # to 0/1), but progress targets can range from tens to low thousands depending on
        # course/config, so standardizing the target matters for stable training.
        self.x_mean = np.zeros(input_dim)
        self.x_std = np.ones(input_dim)
        self.y_mean = 0.0
        self.y_std = 1.0

    def _forward(self, x_std: np.ndarray):
        z1 = x_std @ self.w1 + self.b1
        a1 = np.maximum(z1, 0.0)  # ReLU
        z2 = a1 @ self.w2 + self.b2
        return z1, a1, z2

    def predict(self, genomes):
        """genomes: a single genome (list/1D array) or a 2D array of shape (n, input_dim). Returns
        a single float for one genome, or a 1D array of predictions for a batch. Fast -- two small
        matrix multiplies, no subprocess involved."""
        arr = np.asarray(genomes, dtype=np.float64)
        is_batch = arr.ndim == 2
        x_std = (np.atleast_2d(arr) - self.x_mean) / self.x_std
        _z1, _a1, z2 = self._forward(x_std)
        y = z2.ravel() * self.y_std + self.y_mean
        return y if is_batch else float(y[0])

    def train(self, X: np.ndarray, y: np.ndarray, epochs: int = 200, batch_size: int = 64,
            lr: float = 1e-3, val_fraction: float = 0.15, verbose: bool = True) -> dict:
        """Trains in place via Adam + MSE loss on standardized inputs/targets. Returns a small
        history dict (train/val loss per epoch) for the caller to inspect/print."""
        n = X.shape[0]
        rng = np.random.default_rng(0)
        perm = rng.permutation(n)
        n_val = max(1, int(n * val_fraction)) if n >= 10 else 0
        val_idx, train_idx = perm[:n_val], perm[n_val:]

        self.x_mean = X[train_idx].mean(axis=0)
        self.x_std = X[train_idx].std(axis=0)
        self.x_std[self.x_std < 1e-8] = 1.0  # avoid divide-by-zero for constant genes
        self.y_mean = y[train_idx].mean()
        self.y_std = y[train_idx].std() or 1.0

        X_train = (X[train_idx] - self.x_mean) / self.x_std
        y_train = (y[train_idx] - self.y_mean) / self.y_std
        X_val = (X[val_idx] - self.x_mean) / self.x_std if n_val else None
        y_val = (y[val_idx] - self.y_mean) / self.y_std if n_val else None

        # Adam state
        params = ["w1", "b1", "w2", "b2"]
        m = {p: np.zeros_like(getattr(self, p)) for p in params}
        v = {p: np.zeros_like(getattr(self, p)) for p in params}
        beta1, beta2, eps = 0.9, 0.999, 1e-8
        t = 0

        history = {"train_loss": [], "val_loss": []}
        n_train = X_train.shape[0]
        for epoch in range(epochs):
            epoch_perm = rng.permutation(n_train)
            for start in range(0, n_train, batch_size):
                idx = epoch_perm[start:start + batch_size]
                xb, yb = X_train[idx], y_train[idx][:, None]
                b = xb.shape[0]

                z1, a1, z2 = self._forward(xb)
                # d(MSE)/d(z2)
                d_z2 = 2.0 * (z2 - yb) / b
                d_w2 = a1.T @ d_z2
                d_b2 = d_z2.sum(axis=0)
                d_a1 = d_z2 @ self.w2.T
                d_z1 = d_a1 * (z1 > 0)
                d_w1 = xb.T @ d_z1
                d_b1 = d_z1.sum(axis=0)

                grads = {"w1": d_w1, "b1": d_b1, "w2": d_w2, "b2": d_b2}
                t += 1
                for p in params:
                    g = grads[p]
                    m[p] = beta1 * m[p] + (1 - beta1) * g
                    v[p] = beta2 * v[p] + (1 - beta2) * (g * g)
                    m_hat = m[p] / (1 - beta1 ** t)
                    v_hat = v[p] / (1 - beta2 ** t)
                    setattr(self, p, getattr(self, p) - lr * m_hat / (np.sqrt(v_hat) + eps))

            _z1, _a1, z2_train = self._forward(X_train)
            train_loss = float(np.mean((z2_train.ravel() - y_train) ** 2))
            history["train_loss"].append(train_loss)
            if n_val:
                _z1v, _a1v, z2_val = self._forward(X_val)
                val_loss = float(np.mean((z2_val.ravel() - y_val) ** 2))
                history["val_loss"].append(val_loss)
                if verbose and (epoch % 20 == 0 or epoch == epochs - 1):
                    print(f"epoch {epoch:4d}: train_loss={train_loss:.4f} val_loss={val_loss:.4f}")
            elif verbose and (epoch % 20 == 0 or epoch == epochs - 1):
                print(f"epoch {epoch:4d}: train_loss={train_loss:.4f} (no val set, n={n} too small)")

        return history

    def save(self, path: str) -> None:
        np.savez(path,
                w1=self.w1, b1=self.b1, w2=self.w2, b2=self.b2,
                x_mean=self.x_mean, x_std=self.x_std,
                y_mean=np.array([self.y_mean]), y_std=np.array([self.y_std]),
                meta_json=json.dumps(self.meta.to_dict()) if self.meta else "")

    @staticmethod
    def load(path: str) -> "SurrogateModel":
        data = np.load(path, allow_pickle=False)
        input_dim = data["w1"].shape[0]
        hidden_dim = data["w1"].shape[1]
        meta_json = str(data["meta_json"])
        meta = SurrogateMeta.from_dict(json.loads(meta_json)) if meta_json else None
        model = SurrogateModel(input_dim, hidden_dim, meta)
        model.w1, model.b1 = data["w1"], data["b1"]
        model.w2, model.b2 = data["w2"], data["b2"]
        model.x_mean, model.x_std = data["x_mean"], data["x_std"]
        model.y_mean, model.y_std = float(data["y_mean"][0]), float(data["y_std"][0])
        return model
