#!/usr/bin/env python3
"""Beam search over Kinoko races on the GPU (same idea as beam_search_la.py, much wider).

Every race lives in GPU memory (tools/gpu_spike: the real engine compiled with NVRTC, bit-exact with the CPU build).
A node is a GPU slot plus the inputs that led to it. Each step expands every node by a `--seg`-frame segment in several
ways and steps all children together, one frame per kernel launch, with the policy evaluated on the GPU:

    ghost / ghost_mut   a protected ghost's own next segment, or that segment with a few stick changes
    policy              the policy drives greedily
    policy_T            the policy's inputs sampled at a temperature
    forced              stick forced to a value for the first frames, then the policy takes over

Children are ranked by a `--lookahead`-frame greedy policy rollout (as in beam_search_la.py), the best `--beam` are
kept (diversity key and per-parent cap), protected ghost lines always survive, and survivors are rebuilt by replaying
their segment from the parent slot. The written .rkg is replayed in the CPU kinoko_host as an independent check.

    python tools/gpu_spike/transform.py && python tools/gpu_spike/build_gpu.py     # once, builds the GPU engine
    python tools/gpu_beam.py --ghosts samples/tracks/luigi-circuit/ghosts/lc-top100-pb --policy samples/data/dumps/lc_policy_all463.npz \\
        --out samples/tracks/luigi-circuit/runs/gpu_beam/best.rkg
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from collections import Counter

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gpu_spike"))

import cupy as cp  # noqa: E402

from beam_search_la import FINISH_RATE, load_ghosts, mutate  # noqa: E402
from brute_force import GO_FRAME, decode_rkg, encode_rkg, find_best_start_boost_frame  # noqa: E402
from gpu_engine import ROW_DOUBLES, STAGE, TIMER, Engine, make_inputs  # noqa: E402
from gpu_policy import GpuFeaturizer, GpuPolicy  # noqa: E402
from kinoko_search import DEFAULT_BINARY, KinokoSearch  # noqa: E402

SPEED_BONUS = 2e-4
NEUTRAL = (0, 0, 0.0, 0.0)  # (buttons, trick, stickX, stickY), the order encode_rkg uses
ACCEL = (1, 0, 0.0, 0.0)    # padding past the end of a ghost's recording in a graft


def raw_inputs(frames):
    """[(buttons, trick, sx, sy)] -> int32 (n, 4) in StepInput layout (buttons, sx bits, sy bits, trick)."""
    a = np.zeros((len(frames), 4), np.int32)
    for i, (b, t, sx, sy) in enumerate(frames):
        a[i] = (b, np.float32(sx).view(np.int32), np.float32(sy).view(np.int32), t)
    return a


def tuples(raw):
    """Inverse of raw_inputs for a host array (n, 4)."""
    sx = raw[:, 1].view(np.float32)
    sy = raw[:, 2].view(np.float32)
    return [(int(raw[i, 0]), int(raw[i, 3]), float(sx[i]), float(sy[i])) for i in range(len(raw))]


# scales that turn each component of a state difference into "about 1": used by --match-weight / --state-align
MATCH_POS, MATCH_SPEED, MATCH_HEAD, MATCH_MT = 300.0, 15.0, 0.3, 0.3
MT_FULL = 270.0   # mtCharge at which a mini-turbo is fully charged (stage 2)
MATCH_COMP = 0.03  # a reference point only counts if its race completion is this close


def fwd(qx, qy, qz, qw):
    """x/z of the kart's forward axis from its orientation quaternion."""
    return 2 * (qx * qz + qw * qy), 1 - 2 * (qx * qx + qy * qy)


class Node:
    __slots__ = ("slot", "frame", "inputs", "lineage", "kinds", "value", "info")

    def __init__(self, slot, frame, inputs, lineage, kinds, value=0.0, info=None):
        self.slot, self.frame, self.inputs, self.lineage, self.kinds = slot, frame, inputs, lineage, kinds
        self.value, self.info = value, info


class Search:
    def __init__(self, args, cols):
        self.a = args
        self.cols = cols
        ix = cols.index
        self.ci, self.si, self.mi = ix("completion"), ix("speed"), ix("mushrooms")
        self.pxi, self.pzi, self.dsi = ix("px"), ix("pz"), ix("driftState")
        self.pyi, self.mti, self.qxi = ix("py"), ix("mtCharge"), ix("qx")
        self.fli = ix("flags")  # FrameState flags; bit 14 = BeforeRespawn | InRespawn (fell off the course)
        self.ncols = len(cols)
        self.pcap = args.beam + args.top_ghosts + 4
        cap = 2 * self.pcap + args.work
        self.eng = Engine(cap, r_kib=args.r_kib, course=args.course, character=args.character, vehicle=args.vehicle)
        self.areas = [0, self.pcap]  # parent areas (double buffered), work area at 2 * pcap
        self.work0 = 2 * self.pcap
        self.feat = GpuFeaturizer(cols) if not args.no_policy else None
        self.pol = GpuPolicy(args.policy) if not args.no_policy else None
        self.tracks = None  # per ghost: (frames + 1, 3) host array of (px, pz, completion) after each frame
        self.ref = None     # (points, 8) float32 on the GPU: every ghost's full state per frame, for --match-weight
        self.line = None    # (points, 2) float32 on the GPU: every ghost's (px, pz) path, for --line-weight
        self.rng = cp.random.default_rng(args.seed)       # policy sampling on the GPU
        self.nprng = np.random.default_rng(args.seed)     # forced-stick holds and ghost mutations

    # ------------------------------------------------------------------ helpers on the GPU
    def value_of(self, rows):
        r = rows[:, 1:]
        v = r[:, self.ci] + SPEED_BONUS * r[:, self.si] + self.a.mushroom_bonus * r[:, self.mi]
        if self.line is not None and self.a.line_weight > 0:
            v = v - self.a.line_weight * cp.maximum(self.line_distance(r) - self.a.line_tolerance, 0.0)
        if self.a.mt_weight > 0:  # a charging mini-turbo is worth its eventual boost, which a short lookahead cannot see
            drifting = (r[:, self.fli].astype(cp.int64) & 16) != 0
            v = v + self.a.mt_weight * cp.minimum(r[:, self.mti], MT_FULL) / MT_FULL * drifting
        if self.ref is not None and self.a.match_weight > 0:
            v = v - self.a.match_weight * self.match_dist(r)
        return v

    def match_dist(self, r):
        """Normalised distance (position, speed, heading, mini-turbo charge, drift state) from each race to the nearest
        reference-ghost frame at about the same race completion; capped at 10. 0 = the ghost's own state."""
        qx, qy, qz, qw = (r[:, self.qxi + k] for k in range(4))
        fx, fz = fwd(qx, qy, qz, qw)
        c = cp.stack([r[:, self.pxi], r[:, self.pzi], r[:, self.ci], r[:, self.si], fx, fz,
                      r[:, self.mti] / MT_FULL, r[:, self.dsi]], axis=1).astype(cp.float32)
        R = self.ref
        out = cp.empty(len(c), cp.float32)
        for k in range(0, len(c), 512):
            a = c[k:k + 512, None, :]
            d2 = (((a[..., 0] - R[None, :, 0]) ** 2 + (a[..., 1] - R[None, :, 1]) ** 2) / MATCH_POS ** 2
                  + ((a[..., 3] - R[None, :, 3]) / MATCH_SPEED) ** 2
                  + ((a[..., 4] - R[None, :, 4]) ** 2 + (a[..., 5] - R[None, :, 5]) ** 2) / MATCH_HEAD ** 2
                  + ((a[..., 6] - R[None, :, 6]) / MATCH_MT) ** 2
                  + (a[..., 7] != R[None, :, 7]))
            d2 = cp.where(cp.abs(a[..., 2] - R[None, :, 2]) > MATCH_COMP, 1e6, d2)
            out[k:k + 512] = cp.sqrt(d2.min(axis=1))
        return cp.minimum(out, 10.0).astype(cp.float64)

    def air_update(self, rows, prev_py, air):
        """Frames in a row spent airborne and descending: a kart that keeps doing that is falling off the course, long
        before the respawn state shows it."""
        r = rows[:, 1:]
        py = r[:, self.pyi]
        grounded = (r[:, self.fli].astype(cp.int64) & 4) != 0
        return py, cp.where(~grounded & (py < prev_py - 0.5), air + 1, 0)

    def line_distance(self, r):
        """Distance (x/z plane) from each race to the nearest point of any ghost's path, in chunks to bound memory."""
        pos = cp.stack([r[:, self.pxi], r[:, self.pzi]], axis=1).astype(cp.float32)
        out = cp.empty(len(pos), cp.float32)
        for k in range(0, len(pos), 1024):
            d = ((pos[k:k + 1024, None, :] - self.line[None, :, :]) ** 2).sum(axis=2)
            out[k:k + 1024] = cp.sqrt(d.min(axis=1))
        return out.astype(cp.float64)

    def respawning(self, rows):
        return (rows[:, 1 + self.fli].astype(cp.int64) & (1 << 14)) != 0

    def policy_inputs(self, rows, temps=None):
        feats = self.feat(rows[:, 1:1 + self.ncols])
        return self.pol.act(feats, temps, self.rng)

    def run_fixed(self, first, raw_frames, frame0, track=False):
        """Step races first.. with per-race fixed inputs raw_frames (n, F, 4) (cupy). Returns the final rows and,
        per race, the frame count at which it finished (-1) and its finish time."""
        n, F = raw_frames.shape[:2]
        done = cp.full(n, -1, cp.int32)
        ms = cp.zeros(n, cp.float64)
        rows = None
        # px, pz, completion, then speed, quaternion, driftState, mtCharge (reference state for --match-weight)
        cols3 = [1 + self.pxi, 1 + self.pzi, 1 + self.ci, 1 + self.si] + [1 + self.qxi + k for k in range(4)] \
            + [1 + self.dsi, 1 + self.mti]
        pos = [self.eng.rows(first, n, frame0)[:, cols3]] if track else None
        for f in range(F):
            self.eng.step(first, n, cp.ascontiguousarray(raw_frames[:, f]))
            rows = self.eng.rows(first, n, frame0 + f + 1)
            new = (rows[:, STAGE] >= 4) & (done < 0)
            done = cp.where(new, f + 1, done)
            ms = cp.where(new, rows[:, TIMER], ms)
            if track:
                pos.append(rows[:, cols3])
        if track:
            return rows, done, ms, cp.asnumpy(cp.stack(pos, axis=1))  # (n, F + 1, 10): px, pz, completion, ...
        return rows, done, ms

    # ------------------------------------------------------------------ grafts (no-policy mode)
    def align(self, gi, x, z, completion, info=None):
        """The frame of ghost gi that matches a node: first where the ghost had the node's race completion (so the
        right lap, however far apart the two runs are in time), then the nearest position within --graft-window
        frames of that. A graft continues with that ghost's inputs from there."""
        tr = self.tracks[gi]
        reached = np.maximum.accumulate(tr[:, 2])  # completion can dip briefly; its running max is monotonic
        k0 = int(np.searchsorted(reached, completion))
        k0 = min(k0, len(tr) - 1)
        lo, hi = max(0, k0 - self.a.graft_window), min(len(tr), k0 + self.a.graft_window + 1)
        d = (tr[lo:hi, 0] - x) ** 2 + (tr[lo:hi, 1] - z) ** 2
        if self.a.state_align and info is not None:  # also match speed, heading, mini-turbo charge and drift state
            t = tr[lo:hi]
            q = [float(info[self.qxi + k]) for k in range(4)]
            fxn, fzn = fwd(*q)
            tfx, tfz = fwd(t[:, 4], t[:, 5], t[:, 6], t[:, 7])
            d = (d / MATCH_POS ** 2 + ((t[:, 3] - float(info[self.si])) / MATCH_SPEED) ** 2
                 + ((tfx - fxn) ** 2 + (tfz - fzn) ** 2) / MATCH_HEAD ** 2
                 + ((t[:, 9] - float(info[self.mti])) / MT_FULL / MATCH_MT) ** 2 + (t[:, 8] != float(info[self.dsi])))
        return lo + int(np.argmin(d))

    @staticmethod
    def ghost_slice(gf, start, n):
        return [gf[i] if 0 <= i < len(gf) else ACCEL for i in range(start, start + n)]

    # ------------------------------------------------------------------ one search step
    def expand(self, nodes, ghosts, frame, seg):
        a = self.a
        rng = self.nprng
        L = a.lookahead
        # (parent index, label, kind, temp, forced stick, hold, fixed inputs or None, lookahead inputs or None)
        specs = []
        for pi, n in enumerate(nodes):
            if n.lineage is not None:
                gf = ghosts[n.lineage]["frames"]
                exact = list(gf[frame:frame + seg]) + [NEUTRAL] * max(0, frame + seg - len(gf))
                ahead = self.ghost_slice(gf, frame + seg, L)
                specs.append((pi, "ghost", 0, 0.0, 0.0, 0, exact, ahead))
                for _ in range(a.mutations):
                    specs.append((pi, "ghost_mut", 0, 0.0, 0.0, 0, mutate(exact, rng), ahead))
            if a.grafts:
                x, z, comp = float(n.info[self.pxi]), float(n.info[self.pzi]), float(n.info[self.ci])
                for gi, g in enumerate(ghosts):
                    if gi == n.lineage:
                        continue  # following its own ghost is the "ghost" child above
                    gf = g["frames"]
                    k = self.align(gi, x, z, comp, n.info)
                    for sh in a.graft_shifts:
                        specs.append((pi, "graft", 0, 0.0, 0.0, 0, self.ghost_slice(gf, k + sh, seg),
                                      self.ghost_slice(gf, k + sh + seg, L)))
                    for _ in range(a.graft_mutations):
                        specs.append((pi, "graft_mut", 0, 0.0, 0.0, 0, mutate(self.ghost_slice(gf, k, seg), rng),
                                      self.ghost_slice(gf, k + seg, L)))
            if a.reverse_children and float(n.info[self.si]) < a.stuck_speed:
                back = min(10, seg)
                for st in (-1.0, 0.0, 1.0):  # reverse with the stick one way, then drive out the other way
                    inputs = [(2, 0, st, 0.0)] * back + [(1, 0, -st, 0.0)] * (seg - back)
                    specs.append((pi, "reverse", 0, 0.0, 0.0, 0, inputs, [ACCEL] * L))
            if a.no_policy:
                continue
            specs.append((pi, "policy", 1, 0.0, 0.0, 0, None, None))
            for k in range(a.sampled):
                specs.append((pi, "policy_T", 1, a.temps[k % len(a.temps)], 0.0, 0, None, None))
            for stick in a.forced:
                specs.append((pi, "forced", 2, 0.0, stick, int(rng.integers(4, 11)), None, None))
        if len(specs) > a.work:
            sys.exit(f"{len(specs)} children do not fit in --work {a.work}; lower --beam/--sampled or raise --work")
        C = len(specs)
        w0 = self.work0
        self.eng.copy([nodes[s[0]].slot for s in specs], list(range(w0, w0 + C)))

        kind = np.array([s[2] for s in specs], np.int32)
        temps = cp.asarray(np.array([s[3] for s in specs], np.float32))
        fstick = cp.asarray(np.array([s[4] for s in specs], np.float32))
        hold = cp.asarray(np.array([s[5] for s in specs], np.int32))
        fixed_np = np.zeros((C, seg, 4), np.int32)
        for c, s in enumerate(specs):
            if s[6] is not None:
                fixed_np[c] = raw_inputs(s[6])
        fixed = cp.asarray(fixed_np)
        if self.pol is None and L > 0:
            ahead_np = np.zeros((C, L, 4), np.int32)
            for c, s in enumerate(specs):
                ahead_np[c] = raw_inputs(s[7])
            ahead = cp.asarray(ahead_np)
        is_fixed = cp.asarray(kind == 0)
        is_forced = cp.asarray(kind == 2)

        rows = self.eng.rows(w0, C, frame)
        fell = cp.zeros(C, bool)
        air = cp.zeros(C, cp.int32)
        prev_py = rows[:, 1 + self.pyi]
        slow = cp.zeros(C, cp.int32)
        chosen = cp.zeros((C, seg, 4), cp.int32)
        done = cp.full(C, -1, cp.int32)
        ms = cp.zeros(C, cp.float64)
        for f in range(seg):
            if self.pol is None:
                raw = cp.ascontiguousarray(fixed[:, f])
            else:
                b, t, sx, sy = self.policy_inputs(rows, temps)
                sx = cp.where(is_forced & (f < hold), fstick, sx)
                raw = make_inputs(b, t, sx, sy)
                raw = cp.where(is_fixed[:, None], fixed[:, f], raw)
            chosen[:, f] = raw
            self.eng.step(w0, C, raw)
            rows = self.eng.rows(w0, C, frame + f + 1)
            fell |= self.respawning(rows)
            if a.air_fall_frames > 0:
                prev_py, air = self.air_update(rows, prev_py, air)
                fell |= air >= a.air_fall_frames
            slow += (cp.abs(rows[:, 1 + self.si]) < a.stuck_speed)
            new = (rows[:, STAGE] >= 4) & (done < 0)
            done = cp.where(new, f + 1, done)
            ms = cp.where(new, rows[:, TIMER], ms)
        seg_rows = rows

        if a.lookahead > 0:
            la = cp.full(C, cp.nan, cp.float64)
            for i in range(a.lookahead):
                if self.pol is None:  # no policy: keep following the ghost each child came from
                    self.eng.step(w0, C, cp.ascontiguousarray(ahead[:, i]))
                else:
                    b, t, sx, sy = self.policy_inputs(rows)
                    self.eng.step(w0, C, make_inputs(b, t, sx, sy))
                rows = self.eng.rows(w0, C, frame + seg + i + 1)
                fell |= self.respawning(rows) & cp.isnan(la)
                if a.air_fall_frames > 0:
                    prev_py, air = self.air_update(rows, prev_py, air)
                    fell |= (air >= a.air_fall_frames) & cp.isnan(la)
                slow += (cp.abs(rows[:, 1 + self.si]) < a.stuck_speed) & cp.isnan(la)
                new = (rows[:, STAGE] >= 4) & cp.isnan(la) & (done < 0)
                la = cp.where(new, self.value_of(rows) + FINISH_RATE * (a.lookahead - i - 1), la)
            value = cp.where(cp.isnan(la), self.value_of(rows), la)
        else:
            value = self.value_of(seg_rows)

        if a.stuck_penalty > 0:
            value = value - a.stuck_penalty * slow
        value = value - a.fall_penalty * fell  # a fall costs seconds; never let a forward-moving fall look good
        sr = cp.asnumpy(seg_rows[:, 1:1 + self.ncols])
        return dict(specs=specs, chosen=chosen, chosen_host=cp.asnumpy(chosen), done=cp.asnumpy(done),
                    ms=cp.asnumpy(ms), value=cp.asnumpy(value), seg_rows=sr)

    def select(self, nodes, ex):
        a = self.a
        alive = [c for c in range(len(ex["specs"])) if ex["done"][c] < 0]
        alive.sort(key=lambda c: -ex["value"][c])
        chosen, seen, per_parent, protected = [], set(), Counter(), {}
        for c in alive:
            pi, label = ex["specs"][c][0], ex["specs"][c][1]
            if label == "ghost" and nodes[pi].lineage is not None:
                protected[nodes[pi].lineage] = c
        for c in alive:
            if len(chosen) >= a.beam:
                break
            row = ex["seg_rows"][c]
            pi = ex["specs"][c][0]
            key = (int(row[self.pxi] // 10), int(row[self.pzi] // 10), int(row[self.si] // 2), int(row[self.dsi]))
            if a.mt_weight > 0:  # keep one node per mini-turbo stage so a charging drift is not crowded out
                key += (int(row[self.mti] // 90),)
            if key in seen or per_parent[pi] >= a.max_per_parent:
                continue
            seen.add(key)
            per_parent[pi] += 1
            chosen.append(c)
        for c in protected.values():
            if c not in chosen:
                chosen.append(c)
        return chosen

    def materialize(self, nodes, ex, chosen, frame, seg, area):
        """Rebuild the chosen children in parent area `area`: copy the parent slot, replay the child's segment."""
        first = self.areas[area]
        slots = list(range(first, first + len(chosen)))
        self.eng.copy([nodes[ex["specs"][c][0]].slot for c in chosen], slots)
        idx = cp.asarray(np.array(chosen, np.int64))
        rows, _, _ = self.run_fixed(first, ex["chosen"][idx], frame)
        # determinism check: the replayed state must equal the child's state at the end of its segment
        got = cp.asnumpy(rows[:, 1:1 + self.ncols])
        want = ex["seg_rows"][chosen]
        if not np.array_equal(got.astype(np.float32).view(np.uint32), want.astype(np.float32).view(np.uint32)):
            bad = int(np.nonzero((got != want).any(1))[0][0])
            sys.exit(f"replay of a survivor differs from its expansion (survivor {bad}): GPU copy/replay bug")
        new = []
        for j, c in enumerate(chosen):
            pi, label = ex["specs"][c][0], ex["specs"][c][1]
            p = nodes[pi]
            n = Node(slots[j], frame + seg, p.inputs + tuples(ex["chosen_host"][c]),
                     p.lineage if label == "ghost" else None, p.kinds + [label], float(ex["value"][c]),
                     ex["seg_rows"][c])
            new.append(n)
        return new


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ghosts", help="folder of .rkg ghosts, fastest first by file name")
    ap.add_argument("--ghost-files", nargs="+", help="explicit ghost files (instead of --ghosts / --top-ghosts)")
    ap.add_argument("--policy", help="driving policy (.npz); required unless --no-policy")
    ap.add_argument("--out", required=True)
    ap.add_argument("--top-ghosts", type=int, default=3)
    ap.add_argument("--ghost-offset", type=int, default=0)
    ap.add_argument("--beam", type=int, default=384)
    ap.add_argument("--seg", type=int, default=20)
    ap.add_argument("--max-per-parent", type=int, default=8)
    ap.add_argument("--mutations", type=int, default=4, help="mutated ghost children per ghost-lineage node")
    ap.add_argument("--sampled", type=int, default=8, help="sampled-policy children per node")
    ap.add_argument("--temps", type=float, nargs="+", default=[0.8, 1.3])
    ap.add_argument("--forced", type=float, nargs="*", default=[-1.0, 1.0, 0.0, -3 / 7, 3 / 7])
    ap.add_argument("--lookahead", type=int, default=30)
    ap.add_argument("--mushroom-bonus", type=float, default=0.02)
    ap.add_argument("--no-policy", action="store_true",
                    help="no driving policy: children are ghost / mutated ghost / graft children only, and the "
                         "lookahead keeps following the ghost each child came from")
    ap.add_argument("--grafts", action="store_true",
                    help="every node also tries each ghost's next --seg frames from where that ghost was nearest to "
                         "the node (any frame offset), plus mutations of that; implied by --start-rkg")
    ap.add_argument("--graft-window", type=int, default=40,
                    help="frames either side of the completion-matched frame searched for the nearest position")
    ap.add_argument("--graft-shifts", type=int, nargs="+", default=[-1, 0, 1], help="offsets around the aligned frame")
    ap.add_argument("--graft-mutations", type=int, default=2, help="mutated grafts per node and ghost")
    ap.add_argument("--line-weight", type=float, default=0.0,
                    help="completion subtracted per unit of distance from the nearest point of any ghost's path "
                         "(beyond --line-tolerance): keeps grafted runs where the ghosts' inputs make sense. "
                         "~4e-6 makes 1000 units off the line cost about 5 frames")
    ap.add_argument("--line-tolerance", type=float, default=300.0)
    ap.add_argument("--reverse-children", action="store_true",
                    help="nodes slower than --stuck-speed also try reversing out (brake 10 frames, then drive)")
    ap.add_argument("--stuck-speed", type=float, default=20.0)
    ap.add_argument("--fall-penalty", type=float, default=1.0,
                    help="completion subtracted from a child that enters the respawn state (fell off) during its segment "
                         "or lookahead; 1.0 = a whole lap, i.e. a fall is never preferred")
    ap.add_argument("--mt-weight", type=float, default=0.0,
                    help="completion added for a fully charged mini-turbo while drifting (scaled by mtCharge/270). A "
                         "full charge pays back roughly 10 frames = ~0.008; try 0.004-0.008. Also adds the mini-turbo "
                         "stage to the beam's diversity key")
    ap.add_argument("--air-fall-frames", type=int, default=0,
                    help="treat a child as fallen after this many consecutive airborne, descending frames (0 = off; "
                         "~30 catches a drive off the course long before the respawn state)")
    ap.add_argument("--stuck-penalty", type=float, default=0.0,
                    help="completion subtracted per frame a child spends below --stuck-speed (segment + lookahead); "
                         "~5e-4 = every stuck frame costs about 0.7 of a racing frame")
    ap.add_argument("--match-weight", type=float, default=0.0,
                    help="completion subtracted per unit of normalised state distance (position/300, speed/15, "
                         "heading/0.3 rad, mini-turbo charge, drift state) from the nearest reference-ghost frame at "
                         "the same completion; ~0.002. Keeps a run in a state the ghost's inputs still work from")
    ap.add_argument("--state-align", action="store_true",
                    help="graft alignment also matches speed, heading, mini-turbo charge and drift state, not only "
                         "position, when picking the ghost frame a node continues from")
    ap.add_argument("--start-rkg", help="start the beam from this run's state at --start-frame instead of GO "
                                        "(e.g. an unfinished project); its own inputs are kept up to there")
    ap.add_argument("--start-frame", type=int)
    ap.add_argument("--protect-ghosts", action="store_true",
                    help="with --start-rkg: also seed the ghosts' own lines at --start-frame (protected in the beam)")
    ap.add_argument("--work", type=int, default=5800,
                    help="GPU slots for children (each ~0.57 MiB at the default --r-kib; about 14 children per beam node)")
    ap.add_argument("--r-kib", type=int, default=None,
                    help="private heap per race; default: the course's measured size + 20%% (gpu_spike/slot_sizes.json, "
                         "from validate_all.py). Luigi Circuit needs 482.2 KiB, Mushroom Gorge 1122.6")
    ap.add_argument("--max-steps", type=int, default=100000)
    ap.add_argument("--max-frames", type=int, default=None,
                    help="stop once the beam reaches this frame (default: the slowest ghost's finish + 1200)")
    ap.add_argument("--dump-step", type=int, nargs="*", default=[],
                    help="at these steps, write the best node's inputs to <out>.step<N>.rkg (to inspect the search)")
    ap.add_argument("--save-top", type=int, default=0)
    ap.add_argument("--top-dir")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--log-every", type=int, default=10)
    ap.add_argument("--course", type=int, default=8)
    ap.add_argument("--character", type=int, default=22)
    ap.add_argument("--vehicle", type=int, default=32)
    args = ap.parse_args()
    if not args.no_policy and not args.policy:
        sys.exit("--policy is required unless --no-policy")
    if args.start_rkg:
        if args.start_frame is None:
            sys.exit("--start-rkg needs --start-frame")
        args.grafts = True  # nothing else can extend a run that isn't one of the ghosts without a policy
    if not (args.ghosts or args.ghost_files):
        sys.exit("give --ghosts or --ghost-files")

    with KinokoSearch(DEFAULT_BINARY) as probe:
        cols = probe.cols
    if args.ghost_files:
        from beam_search_la import parse_rkg
        ghosts = [dict(name=os.path.basename(f), frames=decode_rkg(f)[3], time_ms=parse_rkg(open(f, "rb").read())["timeMs"])
                  for f in args.ghost_files]
        args.top_ghosts = len(ghosts)
    else:
        ghosts = load_ghosts(args.ghosts, args.top_ghosts, args.ghost_offset)
    t0 = time.time()
    S = Search(args, cols)
    eng = S.eng
    print(f"GPU engine ready: {eng.capacity} slots x {eng.stride / 2**20:.2f} MiB ({time.time() - t0:.1f}s)", flush=True)

    # --- verify that every ghost replays to its recorded time on the GPU ----------------------------------------
    w0 = S.work0
    G = len(ghosts)
    longest = max(len(g["frames"]) for g in ghosts)
    eng.clone_template(w0, G)
    gr = np.stack([raw_inputs(g["frames"] + [NEUTRAL] * (longest - len(g["frames"]))) for g in ghosts])
    _, done, ms, tracks = S.run_fixed(w0, cp.asarray(gr), 0, track=True)
    S.tracks = [tracks[i, :len(g["frames"]) + 1] for i, g in enumerate(ghosts)]
    if args.match_weight > 0:  # every frame of every ghost from GO on: (x, z, completion, speed, fwd x, fwd z, mt, drift)
        refs = []
        for t in S.tracks:
            t = t[GO_FRAME:]
            fx, fz = fwd(t[:, 4], t[:, 5], t[:, 6], t[:, 7])
            refs.append(np.stack([t[:, 0], t[:, 1], t[:, 2], t[:, 3], fx, fz, t[:, 9] / MT_FULL, t[:, 8]], axis=1))
        S.ref = cp.asarray(np.concatenate(refs).astype(np.float32))
        print(f"state reference: {len(S.ref):,} ghost frames; penalty {args.match_weight:g} per unit", flush=True)
    if args.line_weight > 0:  # every 2nd frame of every ghost from GO on is plenty for a nearest-point distance
        S.line = cp.asarray(np.concatenate([t[GO_FRAME::2, :2] for t in S.tracks]).astype(np.float32))
        print(f"racing line: {len(S.line):,} points from {len(S.tracks)} ghosts; penalty {args.line_weight:g} per unit "
              f"beyond {args.line_tolerance:g}", flush=True)
    for i, g in enumerate(ghosts):
        got = int(ms[i]) if int(done[i]) > 0 else None
        ok = got is not None and abs(got - g["time_ms"]) <= 1
        print(f"  ghost {i}: header {g['time_ms'] / 1000:.3f}s, GPU replay "
              f"{got / 1000 if got else float('nan'):.3f}s  {'OK' if ok else 'MISMATCH'}", flush=True)
        if not ok:
            sys.exit("ghost replay mismatch on the GPU")

    # --- seeds: the scripted start and each protected ghost, all at GO_FRAME; or a given run at --start-frame ----
    if args.start_rkg:
        f0 = args.start_frame
        if decode_rkg(args.start_rkg)[:3] != (args.course, args.character, args.vehicle):
            sys.exit(f"--start-rkg is {decode_rkg(args.start_rkg)[:3]}, not --course/--character/--vehicle")
        start = list(decode_rkg(args.start_rkg)[3][:f0])
        start += [NEUTRAL] * (f0 - len(start))
        seeds = [(None, start)] + ([(g, ghosts[g]["frames"][:f0]) for g in range(G)] if args.protect_ghosts else [])
    else:
        f0 = GO_FRAME
        start_frame = find_best_start_boost_frame()[0]
        scripted = [(1 if f >= start_frame else 0, 0, 0.0, 0.0) for f in range(GO_FRAME)]
        seeds = [(None, scripted)] + [(g, ghosts[g]["frames"][:GO_FRAME]) for g in range(G)]
    eng.clone_template(w0, len(seeds))
    seed_rows, _, _ = S.run_fixed(w0, cp.asarray(np.stack([raw_inputs(s[1]) for s in seeds])), 0)
    seed_info = cp.asnumpy(seed_rows[:, 1:1 + S.ncols])
    area = 0
    eng.copy(list(range(w0, w0 + len(seeds))), list(range(S.areas[area], S.areas[area] + len(seeds))))
    nodes = [Node(S.areas[area] + k, f0, list(inp), lin, [], info=seed_info[k]) for k, (lin, inp) in enumerate(seeds)]
    print(f"seeded {len(nodes)} nodes at frame {f0}"
          + (f" from {os.path.basename(args.start_rkg)} (completion {seed_info[0][S.ci]:.4f})" if args.start_rkg else "")
          + "; ghosts: " + ", ".join(f"{g['time_ms'] / 1000:.3f}s" for g in ghosts), flush=True)

    max_frames = args.max_frames or max(len(g["frames"]) for g in ghosts) + 1200
    best = None  # (time_ms, inputs, kinds)
    finishes = []
    step = 0
    t_search = time.time()
    children_total = 0
    while nodes and step < args.max_steps:
        step += 1
        frame = nodes[0].frame
        ex = S.expand(nodes, ghosts, frame, args.seg)
        children_total += len(ex["specs"])
        for c, s in enumerate(ex["specs"]):
            d = int(ex["done"][c])
            if d > 0:
                t_ms = int(ex["ms"][c])
                p = nodes[s[0]]
                inputs = p.inputs + tuples(ex["chosen_host"][c][:d])
                lineage = p.lineage if s[1] == "ghost" else None
                finishes.append((t_ms, s[1], lineage, inputs))
                if best is None or t_ms < best[0]:
                    best = (t_ms, inputs, p.kinds + [s[1]])
                    print(f"  step {step} frame {frame + d}: FINISH {t_ms / 1000:.3f}s via {s[1]}", flush=True)
        chosen = S.select(nodes, ex)
        if not chosen:
            break
        area ^= 1
        nodes = S.materialize(nodes, ex, chosen, frame, args.seg, area)
        top = max(nodes, key=lambda n: n.value)
        if step in args.dump_step:
            path = f"{os.path.splitext(args.out)[0]}.step{step}.rkg"
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
            open(path, "wb").write(encode_rkg(args.course, args.character, args.vehicle, top.inputs, 0))
            print(f"  dumped the best node at frame {top.frame} ({' '.join(top.kinds[-5:])}) to {path}", flush=True)
        if nodes and nodes[0].frame >= max_frames:
            print(f"stopping: frame {nodes[0].frame} reached --max-frames {max_frames} without a finish", flush=True)
            break
        if step % args.log_every == 0 or step == 1:
            lead = ", ".join(f"{k}:{v}" for k, v in Counter(n.kinds[-1] for n in nodes).items())
            el = time.time() - t_search
            print(f"step {step:3d} frame {top.frame}  best completion {top.info[S.ci]:.4f} speed {top.info[S.si]:.1f} "
                  f"mush {int(top.info[S.mi])}  beam {len(nodes)} [{lead}]  {len(ex['specs'])} children/step  "
                  f"{el:.0f}s ({children_total * (args.seg + args.lookahead) / max(el, 1e-9):,.0f} race-frames/s)",
                  flush=True)
        if best is not None and nodes[0].frame >= len(best[1]):
            break

    if finishes:
        finishes.sort(key=lambda f: f[0])
        print("fastest finishes (ms, last segment origin, protected-ghost id):")
        for f in finishes[:8]:
            print(f"   {f[0]}  {f[1]}  ghost={f[2]}")
        print(f"   ... {len(finishes)} finishes total")
    if best is None:
        print("no run finished")
        return 1
    t_ms, inputs, kinds = best
    print(f"\nbest finish: {t_ms / 1000:.3f}s   ghosts: " + ", ".join(f"{g['time_ms'] / 1000:.3f}s" for g in ghosts)
          + f"   (search {time.time() - t_search:.0f}s, total {time.time() - t0:.0f}s)")
    print("segments of the winning run by origin: " + ", ".join(f"{k}:{v}" for k, v in Counter(kinds).items()))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, "wb").write(encode_rkg(args.course, args.character, args.vehicle, inputs, t_ms))
    print(f"wrote {args.out}")
    if args.save_top:
        os.makedirs(args.top_dir, exist_ok=True)
        seen, written = set(), 0
        for t_f, _k, _l, inp in finishes:
            key = hash(tuple(inp))
            if key in seen:
                continue
            seen.add(key)
            open(os.path.join(args.top_dir, f"run-{t_f:06d}-{written:02d}.rkg"), "wb").write(
                encode_rkg(args.course, args.character, args.vehicle, inp, t_f))
            written += 1
            if written >= args.save_top:
                break
        print(f"saved {written} distinct finished runs to {args.top_dir}")

    # independent check: the written file replayed by the CPU build
    frames = decode_rkg(args.out)[3]
    with KinokoSearch(DEFAULT_BINARY, args.course, args.character, args.vehicle) as race:
        for f in frames:
            race.step(f[0], f[2], f[3], f[1])
            if race.end:
                break
        end = race.end
    ok = end is not None and end["timeMs"] == t_ms
    print(f"CPU round-trip of the written .rkg: {'MATCH' if ok else 'MISMATCH'} "
          f"({end['timeMs'] / 1000 if end else float('nan'):.3f}s)")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
