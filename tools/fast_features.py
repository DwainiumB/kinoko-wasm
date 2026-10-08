"""Single-row version of policy_net.state_features(use_prev_input=False).

state_features is written for batches and costs ~0.65 ms per call even for one row (about 50 small numpy operations plus
list bookkeeping), which dominated a search that asks for features once per simulated frame. FastFeaturizer produces the
same feature vector with a handful of operations: the rotation is built once as a 3x3 matrix and all look-ahead points are
converted in one vectorised step. `python tools/fast_features.py` checks it against state_features on real rows.
"""

from __future__ import annotations

import numpy as np

from policy_net import FLAG_COUNT, PATH_SCALE


class FastFeaturizer:
    def __init__(self, cols):
        ix = cols.index
        self.iq = np.array([ix(n) for n in ("qx", "qy", "qz", "qw")])
        self.iev = np.array([ix(n) for n in ("evx", "evy", "evz")])
        self.iiv = np.array([ix(n) for n in ("ivx", "ivy", "ivz")])
        self.iscalars = np.array([ix(n) for n in ("speed", "speedRatio", "mtCharge", "hopStickX", "kclSpeedFactor")])
        self.i_drift, self.i_flags, self.i_mush = ix("driftState"), ix("flags"), ix("mushrooms")
        self.ipos = np.array([ix("px"), ix("py"), ix("pz")])
        self.i_comp, self.i_lap = ix("completion"), ix("lap")
        self.px, self.pz = ix("px"), ix("pz")
        self.bits = np.arange(FLAG_COUNT)
        self.eye3 = np.eye(3)
        self.pos_scale = np.array([1 / 30000.0, 1 / 5000.0, 1 / 30000.0])
        self.has_path = "pmx0" in cols
        if self.has_path:
            k = 0
            while f"pmx{k}" in cols:
                k += 1
            self.k = k
            # x and z column of the middle, left and right point of each look-ahead checkpoint: shape (k, 3)
            self.ix_pts = np.array([[ix(f"pmx{j}"), ix(f"plx{j}"), ix(f"prx{j}")] for j in range(k)])
            self.iz_pts = np.array([[ix(f"pmz{j}"), ix(f"plz{j}"), ix(f"prz{j}")] for j in range(k)])

    def __call__(self, row: np.ndarray) -> np.ndarray:
        """Features of one state row, shape (1, F) float32, in the same order as state_features."""
        q = row[self.iq]
        u, w = q[:3], q[3]
        # Rotation by q: v + w*t + u x t with t = 2 u x v  ==  R v with R = I + 2w K + 2 K^2, K the cross-product matrix of u.
        K = np.array([[0.0, -u[2], u[1]], [u[2], 0.0, -u[0]], [-u[1], u[0], 0.0]])
        K2 = np.outer(u, u) - u.dot(u) * self.eye3
        R = self.eye3 + 2.0 * w * K + 2.0 * K2
        fwd, up, right = R[:, 2], R[:, 1], R[:, 0]
        ev_local = R.T @ row[self.iev]  # rotation by the conjugate quaternion is the transpose

        flags = int(row[self.i_flags])
        parts = [fwd, up, right, ev_local, row[self.iiv], row[self.iscalars],
                 self.eye3[min(max(int(row[self.i_drift]), 0), 2)],
                 ((flags >> self.bits) & 1).astype(np.float64),
                 [row[self.i_mush]], row[self.ipos] * self.pos_scale]
        comp = row[self.i_comp]
        parts += [[comp - np.floor(comp)], [row[self.i_lap]]]

        if self.has_path:
            f = fwd[[0, 2]]
            f = f / max(np.linalg.norm(f), 1e-6)
            r = right[[0, 2]]
            r = r / max(np.linalg.norm(r), 1e-6)
            dx = row[self.ix_pts] - row[self.px]  # (k, 3): middle, left, right
            dz = row[self.iz_pts] - row[self.pz]
            lat = (dx * r[0] + dz * r[1]) / PATH_SCALE
            ahead = (dx * f[0] + dz * f[1]) / PATH_SCALE
            bearing = np.arctan2(lat[:, 0], ahead[:, 0])
            # per checkpoint: pathMid (lat, ahead, bearing) then pathEdges (left lat, left ahead, right lat, right ahead)
            path = np.column_stack([lat[:, 0], ahead[:, 0], bearing, lat[:, 1], ahead[:, 1], lat[:, 2], ahead[:, 2]])
            parts.append(path.ravel())

        return np.concatenate([np.asarray(p, dtype=np.float64).ravel() for p in parts]).astype(np.float32)[None, :]


if __name__ == "__main__":
    import time

    from policy_net import Policy, state_features

    d = np.load("samples/data/dumps/lc-all463.npz")
    cols = [str(c) for c in d["columns"]]
    data = d["data"]
    rng = np.random.default_rng(0)
    idx = np.sort(rng.choice(len(data), 20000, replace=False))
    rows = data[idx].astype(np.float64)
    ref, _ = state_features(rows, cols, False)
    ff = FastFeaturizer(cols)
    got = np.concatenate([ff(r) for r in rows])
    diff = np.abs(ref - got)
    print(f"{len(rows)} real rows, {ref.shape[1]} features: max abs diff {diff.max():.2e}, mean {diff.mean():.2e}")
    pol = Policy.load("samples/data/dumps/lc_policy_all463.npz")
    a, b = pol.act(ref), pol.act(got)
    same = {k: float((a[k] == b[k]).mean()) for k in a}
    print("greedy policy output identical on:", {k: f"{100 * v:.2f}%" for k, v in same.items()})
    t = time.perf_counter()
    for r in rows[:3000]:
        ff(r)
    fast = (time.perf_counter() - t) / 3000 * 1000
    t = time.perf_counter()
    for r in rows[:3000]:
        state_features(r[None, :], cols, False)
    slow = (time.perf_counter() - t) / 3000 * 1000
    print(f"per call: fast {fast:.3f} ms, original {slow:.3f} ms  ({slow / fast:.1f}x)")
