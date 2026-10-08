"""Features and a small numpy MLP policy for imitating ghost inputs.

Same footprint as surrogate.py: numpy only. The model maps the kart state after frame t-1 to the
controller input on frame t, as four classification heads sharing one trunk:

    stickX   softmax over the distinct stick values seen in training (the Wii stick is quantized)
    stickY   same
    trick    softmax over trick ids (0 = none)
    buttons  one sigmoid per button bit seen in training

`featurize` and `Policy.act` are what a search would call to propose inputs; train_policy.py
trains and evaluates the model. State columns come from tools/dump_ghosts.py (see its docstring).
"""

from __future__ import annotations

import numpy as np

# Columns that describe the controller input (targets, or the "previous input" feature).
INPUT_COLUMNS = ("stickX", "stickY", "buttons", "trick")
FLAG_COUNT = 18  # bits in the `flags` column, see dump_ghosts.FLAG_BITS


def _rotate(q: np.ndarray, v: np.ndarray) -> np.ndarray:
    """Rotate vectors v (n,3) by quaternions q (n,4) laid out as (x, y, z, w)."""
    u, w = q[:, :3], q[:, 3:4]
    t = 2.0 * np.cross(u, v)
    return v + w * t + np.cross(u, t)


def _col(data, cols, name):
    return data[:, cols.index(name)].astype(np.float64)


def state_features(data: np.ndarray, cols: list[str], use_prev_input: bool) -> tuple[np.ndarray, list[str]]:
    """Features for every row of `data`, describing the state AFTER that row's frame."""
    n = len(data)
    c = lambda name: _col(data, cols, name)
    feats, names = [], []

    def add(name, v):
        feats.append(np.asarray(v, dtype=np.float64).reshape(n, -1))
        names.extend([name] if feats[-1].shape[1] == 1 else [f"{name}{i}" for i in range(feats[-1].shape[1])])

    q = np.stack([c("qx"), c("qy"), c("qz"), c("qw")], axis=1)
    axes = {"fwd": (0, 0, 1), "up": (0, 1, 0), "right": (1, 0, 0)}
    for name, a in axes.items():
        add(name, _rotate(q, np.tile(np.array(a, dtype=np.float64), (n, 1))))

    # External velocity in the kart's own frame (rotate by the inverse = conjugate).
    qc = q * np.array([-1.0, -1.0, -1.0, 1.0])
    add("evLocal", _rotate(qc, np.stack([c("evx"), c("evy"), c("evz")], axis=1)))
    add("iv", np.stack([c("ivx"), c("ivy"), c("ivz")], axis=1))

    add("speed", c("speed"))
    add("speedRatio", c("speedRatio"))
    add("mtCharge", c("mtCharge"))
    add("hopStickX", c("hopStickX"))
    add("kclSpeedFactor", c("kclSpeedFactor"))
    add("drift", np.eye(3)[c("driftState").astype(int).clip(0, 2)])
    flags = c("flags").astype(np.int64)
    add("flag", ((flags[:, None] >> np.arange(FLAG_COUNT)) & 1))
    add("mushrooms", c("mushrooms"))

    # Position and progress. These tie the policy to one track; fine for a single-track first
    # pass, but the features to swap for a transferable policy (path-relative ones) go here.
    add("pos", np.stack([c("px") / 30000.0, c("py") / 5000.0, c("pz") / 30000.0], axis=1))
    comp = c("completion")
    add("lapFrac", comp - np.floor(comp))
    add("lap", c("lap"))

    if "pmx0" in cols:
        _add_path_features(add, q, c, cols)

    if use_prev_input:
        add("prevStick", np.stack([c("stickX"), c("stickY")], axis=1))
        add("prevButtons", (c("buttons").astype(np.int64)[:, None] >> np.arange(7)) & 1)
        add("prevTrick", np.eye(5)[c("trick").astype(int).clip(0, 4)])

    return np.concatenate(feats, axis=1).astype(np.float32), names


PATH_SCALE = 3000.0  # world units; checkpoint spacing on Luigi Circuit is a few hundred to ~1000


def _add_path_features(add, q, c, cols):
    """The road ahead in the kart's own horizontal frame.

    For each look-ahead checkpoint k the dump holds the world x/z of its middle and both edges.
    Each point becomes (lateral, ahead) relative to the kart, where `ahead` is along the kart's
    forward axis and `lateral` along its right axis (both projected on the ground plane), plus the
    bearing atan2(lateral, ahead) of the middle point. This is what lets a policy steer toward the
    road instead of memorizing x/z coordinates, and it is the same on any track.
    """
    n = len(q)
    ones = np.ones((n, 1))
    zeros = np.zeros((n, 1))
    fwd = _rotate(q, np.hstack([zeros, zeros, ones]))[:, [0, 2]]
    right = _rotate(q, np.hstack([ones, zeros, zeros]))[:, [0, 2]]
    fwd /= np.maximum(np.linalg.norm(fwd, axis=1, keepdims=True), 1e-6)
    right /= np.maximum(np.linalg.norm(right, axis=1, keepdims=True), 1e-6)
    px, pz = c("px"), c("pz")

    def local(xname, zname):
        dx, dz = c(xname) - px, c(zname) - pz
        return (dx * right[:, 0] + dz * right[:, 1]) / PATH_SCALE, (dx * fwd[:, 0] + dz * fwd[:, 1]) / PATH_SCALE

    k = 0
    while f"pmx{k}" in cols:
        mlat, mahead = local(f"pmx{k}", f"pmz{k}")
        llat, lahead = local(f"plx{k}", f"plz{k}")
        rlat, rahead = local(f"prx{k}", f"prz{k}")
        add(f"pathMid{k}", np.stack([mlat, mahead, np.arctan2(mlat, mahead)], axis=1))
        add(f"pathEdges{k}", np.stack([llat, lahead, rlat, rahead], axis=1))
        k += 1


def make_pairs(data: np.ndarray, cols: list[str], ghost_idx: np.ndarray, use_prev_input: bool):
    """(features, target rows, ghost id, previous-input rows) with state[t-1] -> input[t].

    The first frame of each ghost has no preceding state and is dropped. When `use_prev_input` is on
    the features also contain input[t-1], which makes the task much easier (inputs are smooth), so
    train_policy.py always reports the repeat-previous-input baseline next to the model's accuracy.
    """
    feats, names = state_features(data, cols, use_prev_input)
    keep = np.nonzero(ghost_idx[1:] == ghost_idx[:-1])[0] + 1  # rows t that have a t-1 in the same ghost
    x = feats[keep - 1]
    tgt = data[keep]
    prev = data[keep - 1]
    return x, tgt, ghost_idx[keep], prev, names


class Targets:
    """Maps raw input columns to class ids / bit labels and back."""

    def __init__(self, stick_x_vals, stick_y_vals, trick_vals, bit_ids):
        self.stick_x_vals = np.asarray(stick_x_vals, dtype=np.float32)
        self.stick_y_vals = np.asarray(stick_y_vals, dtype=np.float32)
        self.trick_vals = np.asarray(trick_vals, dtype=np.int64)
        self.bit_ids = list(bit_ids)

    @classmethod
    def fit(cls, rows: np.ndarray, cols: list[str], min_bit_frames: int = 20):
        buttons = _col(rows, cols, "buttons").astype(np.int64)
        bits = [b for b in range(16) if ((buttons >> b) & 1).sum() >= min_bit_frames]
        return cls(np.unique(_col(rows, cols, "stickX").round(3)), np.unique(_col(rows, cols, "stickY").round(3)),
                   np.unique(_col(rows, cols, "trick").astype(np.int64)), bits)

    def encode(self, rows: np.ndarray, cols: list[str]):
        def nearest(vals, v):
            return np.abs(v[:, None] - vals[None, :]).argmin(axis=1)

        buttons = _col(rows, cols, "buttons").astype(np.int64)
        return {
            "stickX": nearest(self.stick_x_vals, _col(rows, cols, "stickX")),
            "stickY": nearest(self.stick_y_vals, _col(rows, cols, "stickY")),
            "trick": nearest(self.trick_vals.astype(np.float64), _col(rows, cols, "trick")),
            "bits": np.stack([(buttons >> b) & 1 for b in self.bit_ids], axis=1).astype(np.float32),
        }

    @property
    def sizes(self):
        return len(self.stick_x_vals), len(self.stick_y_vals), len(self.trick_vals), len(self.bit_ids)


def _softmax(z):
    z = z - z.max(axis=1, keepdims=True)
    e = np.exp(z)
    return e / e.sum(axis=1, keepdims=True)


def _sigmoid(z):
    return 1.0 / (1.0 + np.exp(-np.clip(z, -30, 30)))


class Policy:
    """Two-hidden-layer ReLU MLP with softmax/sigmoid heads, trained with Adam."""

    def __init__(self, n_in: int, targets: Targets, hidden: int = 256, seed: int = 0):
        rng = np.random.default_rng(seed)
        self.targets = targets
        self.n_out = sum(targets.sizes)
        dims = [n_in, hidden, hidden, self.n_out]
        self.p = {}
        for i in range(3):
            self.p[f"W{i}"] = (rng.standard_normal((dims[i], dims[i + 1])) * np.sqrt(2.0 / dims[i])).astype(np.float32)
            self.p[f"b{i}"] = np.zeros(dims[i + 1], dtype=np.float32)
        self.p["W2"] *= 0.1
        self.mean = np.zeros(n_in, dtype=np.float32)
        self.std = np.ones(n_in, dtype=np.float32)

    # ---- forward -------------------------------------------------------------------------------
    def _forward(self, x):
        h0 = np.maximum(x @ self.p["W0"] + self.p["b0"], 0)
        h1 = np.maximum(h0 @ self.p["W1"] + self.p["b1"], 0)
        return h0, h1, h1 @ self.p["W2"] + self.p["b2"]

    def _split(self, logits):
        kx, ky, kt, kb = self.targets.sizes
        a, b, c = kx, kx + ky, kx + ky + kt
        return logits[:, :a], logits[:, a:b], logits[:, b:c], logits[:, c:]

    def predict_proba(self, x):
        """Per-head probabilities for raw (unnormalized) features x."""
        _, _, logits = self._forward((x - self.mean) / self.std)
        lx, ly, lt, lb = self._split(logits)
        return {"stickX": _softmax(lx), "stickY": _softmax(ly), "trick": _softmax(lt), "bits": _sigmoid(lb)}

    def act(self, x, rng=None, temperature=1.0):
        """An input for each row: dict with stickX, stickY, trick and buttons (bitmask).

        Without `rng` this is the most likely input (deterministic). With `rng` each head is sampled
        from its predicted distribution, sharpened or flattened by `temperature` (1 = as predicted),
        which is how DAgger rollouts get varied trajectories out of one policy."""
        pr = self.predict_proba(x)
        t = self.targets
        n = len(x)

        def pick(p):
            if rng is None:
                return p.argmax(1)
            q = p ** (1.0 / max(temperature, 1e-3))
            q /= q.sum(axis=1, keepdims=True)
            u = rng.random((n, 1))
            return np.minimum((q.cumsum(axis=1) < u).sum(axis=1), q.shape[1] - 1)

        buttons = np.zeros(n, dtype=np.int64)
        for j, b in enumerate(t.bit_ids):
            on = (pr["bits"][:, j] > 0.5) if rng is None else (rng.random(n) < pr["bits"][:, j])
            buttons |= on.astype(np.int64) << b
        return {"stickX": t.stick_x_vals[pick(pr["stickX"])], "stickY": t.stick_y_vals[pick(pr["stickY"])],
                "trick": t.trick_vals[pick(pr["trick"])], "buttons": buttons}

    # ---- training ------------------------------------------------------------------------------
    def loss_and_grads(self, x, y, class_weights=None):
        h0, h1, logits = self._forward(x)
        lx, ly, lt, lb = self._split(logits)
        n = len(x)
        dl = np.zeros_like(logits)
        kx, ky, kt, _ = self.targets.sizes
        offsets = [0, kx, kx + ky, kx + ky + kt]
        loss = 0.0
        for off, lg, key in zip(offsets[:3], (lx, ly, lt), ("stickX", "stickY", "trick")):
            p = _softmax(lg)
            idx = y[key]
            loss += -np.log(p[np.arange(n), idx] + 1e-9).mean()
            d = p
            d[np.arange(n), idx] -= 1.0
            dl[:, off:off + lg.shape[1]] = d / n
        pb = _sigmoid(lb)
        yb = y["bits"]
        loss += -(yb * np.log(pb + 1e-9) + (1 - yb) * np.log(1 - pb + 1e-9)).mean(axis=0).sum()
        dl[:, offsets[3]:] = (pb - yb) / n

        g = {}
        g["W2"], g["b2"] = h1.T @ dl, dl.sum(0)
        d1 = (dl @ self.p["W2"].T) * (h1 > 0)
        g["W1"], g["b1"] = h0.T @ d1, d1.sum(0)
        d0 = (d1 @ self.p["W1"].T) * (h0 > 0)
        g["W0"], g["b0"] = x.T @ d0, d0.sum(0)
        return loss, g

    def fit_normalizer(self, x):
        self.mean = x.mean(0).astype(np.float32)
        self.std = np.maximum(x.std(0), 1e-3).astype(np.float32)

    def eval_loss(self, x, y):
        xn = (x - self.mean) / self.std
        pr = self.predict_proba(x)
        n = len(x)
        loss = 0.0
        for key in ("stickX", "stickY", "trick"):
            loss += -np.log(pr[key][np.arange(n), y[key]] + 1e-9).mean()
        pb, yb = pr["bits"], y["bits"]
        loss += -(yb * np.log(pb + 1e-9) + (1 - yb) * np.log(1 - pb + 1e-9)).mean(axis=0).sum()
        return float(loss)

    # ---- persistence ---------------------------------------------------------------------------
    def save(self, path, feature_names, use_prev_input):
        t = self.targets
        np.savez_compressed(path, mean=self.mean, std=self.std, feature_names=np.array(feature_names),
                use_prev_input=np.array(use_prev_input), stick_x_vals=t.stick_x_vals, stick_y_vals=t.stick_y_vals,
                trick_vals=t.trick_vals, bit_ids=np.array(t.bit_ids), **self.p)

    @classmethod
    def load(cls, path):
        d = np.load(path)
        t = Targets(d["stick_x_vals"], d["stick_y_vals"], d["trick_vals"], d["bit_ids"].tolist())
        pol = cls(len(d["mean"]), t, hidden=d["W0"].shape[1])
        pol.p = {k: d[k] for k in pol.p}
        pol.mean, pol.std = d["mean"], d["std"]
        pol.use_prev_input = bool(d["use_prev_input"])
        pol.feature_names = [str(n) for n in d["feature_names"]]
        return pol
