#!/usr/bin/env python3
"""tools/brute_force.py's search, evaluated on the GPU engine (tools/gpu_spike) instead of kinoko_host subprocesses.

Same genome, same segment chaining, same fitness() -- the scoring function is brute_force.fitness itself, called on
result dicts built from GPU state rows exactly the way kinoko_host's RESULT line is printed (same fields, same %.6f
rounding, same int(raceCompletion * 10000)). Takes every brute_force.py argument, plus a few GPU-only ones below.

What is shared with brute_force.py, unchanged:
    SearchConfig, fitness(), frames_from_genome() (vectorized here and checked against the original every
    generation), mutate()'s distribution (vectorized), genome seeding (--seed-rkg), compute_progress_reference /
    compute_heading_reference (fed by GPU rollouts instead of subprocesses), the .rkg export.

What is different, on purpose:
    - one generation evaluates --batch children at once (the CPU loop: --parallel-workers, usually 8). With
      --parents 1 (default) this is the CPU's hill climb -- every child is a mutation of the current best, the best
      child replaces it only if strictly better -- just much wider. --parents k keeps the best k distinct genomes
      instead ((mu + lambda)), children spread round-robin over them.
    - the locked prefix (--prefix-rkg), or the 172 neutral intro frames when there is none, is simulated ONCE into a
      base slot; every child starts as a copy of that slot (k_copy_slots) and only the searched tail is simulated.
    - the heading checkpoint (--heading-checkpoint-frame) is read from the same rollout, no second evaluation.
    - --eval-log-path / --surrogate-path are not supported (a surrogate pre-filter is pointless at this speed).

Edit mode (--edit-base-rkg, class EditSearch): instead of the genome, the individual is a real input stream -- a base
ghost's frames after the prefix -- and children are local edits of it (stick X/Y runs, moving a button/trick change by
1-2 frames, shifting a block by a frame, trick direction 1-4, hop/brake/drift on/off). This reaches what the genome
cannot (drift, trick direction, stickY). Same evaluation, fitness, loop, CPU checks and export.

Any course / character / vehicle: the GPU engine loads Common.szs plus that course's archive (see
tools/gpu_spike/validate_all.py for the per-track exactness and slot-size checks).

    python tools/gpu_brute.py --check-parity 24 --course 8 --character 22 --vehicle 32 --max-frames 600 ...
    python tools/gpu_brute.py --course 8 --character 22 --vehicle 32 --fitness-metric progress \\
        --reference-rkg samples/tracks/luigi-circuit/ghosts/LC_3lap_67.788_ref.rkg --prefix-rkg samples/tracks/luigi-circuit/ghosts/LC_41.105s_41b3f3e.rkg \\
        --prefix-frame-count 1250 --progress-start-frame 1250 --max-frames 1450 --step 5 --n-points 40 \\
        --search-trick --iterations 300 --out samples/tracks/luigi-circuit/runs/gpu_brute/best.rkg
    python tools/gpu_brute.py --course 8 --character 22 --vehicle 32 --fitness-metric progress \\
        --reference-rkg samples/tracks/luigi-circuit/ghosts/LC_3lap_67.788_ref.rkg --prefix-rkg samples/tracks/luigi-circuit/ghosts/LC_41.105s_41b3f3e.rkg \\
        --prefix-frame-count 1250 --progress-start-frame 1250 --max-frames 1450 --min-boost-charge 0.94 \\
        --max-boost-charge 0.95 --speed-bonus-weight 5 --edit-base-rkg samples/tracks/luigi-circuit/ghosts/LC_3lap_67.788_ref.rkg \\
        --edit-base-shifts=-1,0,1 --parents 4 --iterations 120 --verify-every 30 --out samples/tracks/luigi-circuit/runs/gpu_brute/edits.rkg
"""

from __future__ import annotations

import os
import random
import sys
import threading
import time
from typing import Callable, Optional

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gpu_spike"))

import cupy as cp  # noqa: E402

import brute_force as bf  # noqa: E402
from gpu_engine import ROW_DOUBLES, STAGE, TIMER, Engine  # noqa: E402

CPU_BINARY = os.path.join(ROOT, "build-gpu-spike", "kinoko_host.exe")  # same heap layout as the GPU build

# host/FrameState.cc WriteStateHeader, in order; k_rows puts column i at row index 1 + i.
FRAME_STATE_COLS = ("frame,stage,timerMs,raceFrame,px,py,pz,qx,qy,qz,qw,mqx,mqy,mqz,mqw,evx,evy,evz,ivx,ivy,ivz,"
                    "speed,speedRatio,driftState,mtCharge,hopStickX,kclSpeedFactor,flags,stickX,stickY,buttons,trick,"
                    "completion,checkpoint,lap,wrongWay,mushrooms").split(",")
COL = {name: 1 + i for i, name in enumerate(FRAME_STATE_COLS)}
# runtime.cu k_rows: the extra fields KBruteSystem::reportResult prints that FrameState does not have.
BOOST_CHARGE, BOOST_MULT, FRONT_X, FRONT_Y, FRONT_Z, IN_STUNT, IN_TRICK = range(110, 117)
FINISH_GLOBAL = 4  # RaceManager::Stage::FinishGlobal, where KBruteSystem::calcEnd stops
GATE_PENALTY = 1_000_000_000.0  # what brute_force.fitness adds for a failed completion / boost gate
NEUTRAL = (0, 0, 0.0, 0.0)


def _f6(v) -> float:
    """What a value goes through on the CPU path: printf("%.6f") in kinoko_host, float() in parse_result."""
    return float("%.6f" % v)


def result_from_row(row, frames_run: int) -> dict:
    """A GPU state row (host float64, ROW_DOUBLES) -> the dict brute_force.parse_result builds from kinoko_host's
    RESULT line. lap1-3 and goFrame are left out: nothing in fitness() reads them and the rows don't carry them."""
    finished = int(row[STAGE]) == FINISH_GLOBAL
    return {
        "finished": finished,
        "frames": int(frames_run),
        "timeMs": int(row[TIMER]) if finished else -1,
        "completion": int(np.float32(row[COL["completion"]]) * np.float32(10000.0)),
        "wrongWay": bool(row[COL["wrongWay"]]),
        "speed": _f6(row[COL["speed"]]),
        "posX": _f6(row[COL["px"]]), "posY": _f6(row[COL["py"]]), "posZ": _f6(row[COL["pz"]]),
        "boostCharge": _f6(row[BOOST_CHARGE]), "boostMultiplier": _f6(row[BOOST_MULT]),
        "rotW": _f6(row[COL["qw"]]), "rotX": _f6(row[COL["qx"]]), "rotY": _f6(row[COL["qy"]]),
        "rotZ": _f6(row[COL["qz"]]),
        "frontX": _f6(row[FRONT_X]), "frontY": _f6(row[FRONT_Y]), "frontZ": _f6(row[FRONT_Z]),
        "inStunt": int(row[IN_STUNT]), "inTrick": int(row[IN_TRICK]),
    }


def capped_progress(pos, progress_ref) -> float:
    """brute_force.piecewise_progress with each segment's projection clipped to that segment's length, so the
    result can't exceed the reference line's total length."""
    best = float("-inf")
    for seg in progress_ref["segments"]:
        projected = sum((p - st) * u for p, st, u in zip(pos, seg["start"], seg["unit"]))
        best = max(best, seg["before"] + min(projected, seg["length"]))
    return best


def raw_inputs(frames) -> np.ndarray:
    """[(buttons, trick, sx, sy)] -> int32 (n, 4) in runtime.cu StepInput layout (buttons, sx bits, sy bits, trick)."""
    a = np.zeros((len(frames), 4), np.int32)
    for i, (b, t, sx, sy) in enumerate(frames):
        a[i] = (b, np.float32(sx).view(np.int32), np.float32(sy).view(np.int32), t)
    return a


class GpuRunner:
    """Batched kinoko_host-brute equivalents on the GPU engine. Slot 0 is the search's base slot, the rest are work."""

    def __init__(self, capacity: int, r_kib: Optional[int], combo=(8, 22, 32)):
        self.eng = Engine(capacity + 1, r_kib=r_kib, course=combo[0], character=combo[1], vehicle=combo[2])
        self.capacity = capacity
        self.race_frames = 0

    def rollout(self, first: int, n: int, inputs, start: int, ends, capture=()):
        """Races in slots first..first+n-1 are at frame `start`; inputs is cupy int32 (T, n, 4), row t = the inputs
        of frame start + t. A race stops (its state is frozen) when it reaches FinishGlobal or its own end frame
        (cupy int32 ends), like KBruteSystem::calcEnd. Returns (final rows (n, ROW_DOUBLES) cupy, cupy frames run,
        {frame: rows at that frame, frozen the same way} for each frame in `capture`)."""
        eng = self.eng
        final = eng.rows(first, n, start)
        stopped = (ends <= start) | (final[:, STAGE] == FINISH_GLOBAL)
        stop_frame = cp.where(stopped, start, -1).astype(cp.int32)
        captured = {}
        last = int(cp.max(ends).get())
        for f in range(start, last):
            if (f - start) % 32 == 31 and bool(stopped.all()):
                break
            eng.step(first, n, inputs[f - start])
            rows = eng.rows(first, n, f + 1)
            new = ~stopped & ((rows[:, STAGE] == FINISH_GLOBAL) | (ends <= f + 1))
            final = cp.where(new[:, None], rows, final)
            stop_frame = cp.where(new, f + 1, stop_frame)
            stopped |= new
            if f + 1 in capture:
                captured[f + 1] = final.copy() if bool(stopped.all()) else cp.where(stopped[:, None], final, rows)
            self.race_frames += n
        return final, stop_frame, captured

    def run_many(self, jobs):
        """jobs [(frames, max_frames)] from frame 0 -> kinoko_host-brute result dicts, the same as
        brute_force._cpu_run_many (frames past the end of a job's list are neutral, as in KBruteSystem::calc)."""
        out = []
        for k in range(0, len(jobs), self.capacity):
            chunk = jobs[k:k + self.capacity]
            n = len(chunk)
            T = max(m for _f, m in chunk)
            raw = np.zeros((T, n, 4), np.int32)
            for j, (frames, _m) in enumerate(chunk):
                if frames:
                    raw[:min(len(frames), T), j] = raw_inputs(frames[:T])
            self.eng.clone_template(1, n)
            ends = cp.asarray(np.array([m for _f, m in chunk], np.int32))
            final, run, _ = self.rollout(1, n, cp.asarray(raw), 0, ends)
            final, run = cp.asnumpy(final), cp.asnumpy(run)
            out += [result_from_row(final[j], run[j]) for j in range(n)]
        return out


# ---------------------------------------------------------------------------------------------- vectorized genome ops

def genome_layout(cfg: bf.SearchConfig):
    """(gene count, accel channel offset or None, start-boost gene index or None, trick offset or None), the layout
    brute_force._channels_from_genome documents."""
    n = cfg.n_points
    accel = n if cfg.search_accelerate else None
    boost = n if (cfg.search_start_boost and not cfg.search_accelerate) else None
    trick_off = n + (n if cfg.search_accelerate else (1 if boost is not None else 0))
    trick = trick_off if cfg.search_trick else None
    count = bf.genome_gene_count(n, cfg.search_start_boost, cfg.search_accelerate, cfg.search_trick)
    return count, accel, boost, trick


def frames_batch(genomes: np.ndarray, cfg: bf.SearchConfig, seg_start: int, first: int, total: int):
    """brute_force.frames_from_genome for frames first..total-1 of every genome at once (first must be past the
    prefix / intro region). Returns (buttons, trick, stickX) arrays of shape (N, total - first); stickY is 0."""
    n = cfg.n_points
    _count, accel, boost, trick = genome_layout(cfg)
    i = np.arange(first, total)
    pos = (i - seg_start) / cfg.step
    idx = np.minimum(pos.astype(np.int64), n - 1)
    nxt = np.minimum(idx + 1, n - 1)
    t = np.where(idx != nxt, pos - idx, 0.0)
    steer = genomes[:, :n]
    sx = steer[:, idx] * (1 - t) + steer[:, nxt] * t
    sx = np.maximum(-1.0, np.minimum(1.0, sx))
    if cfg.ternary_steering:
        sx = np.where(sx >= 0.5, 1.0, np.where(sx <= -0.5, -1.0, 0.0))
    else:
        sx = (np.clip(np.rint(sx * 7 + 7), 0, 14) - 7) / 7.0
    if accel is not None:
        buttons = (genomes[:, accel:accel + n][:, idx] > 0).astype(np.int32) * bf.BUTTON_ACCELERATE
    else:
        if boost is not None:
            frac = np.maximum(0.0, np.minimum(1.0, genomes[:, boost]))
            start = bf.COUNTDOWN_START_FRAME + frac * (bf.GO_FRAME - bf.COUNTDOWN_START_FRAME)
        else:
            start = np.full(len(genomes), float(bf.COUNTDOWN_START_FRAME))
        buttons = (i[None, :] >= start[:, None]).astype(np.int32) * bf.BUTTON_ACCELERATE
    if trick is not None:
        tricks = (genomes[:, trick:trick + n][:, idx] > 0).astype(np.int32) * bf.System_Trick_Up
    else:
        tricks = np.zeros_like(buttons)
    return buttons, tricks, sx


def pack_inputs(buttons, tricks, sx, sy=None) -> np.ndarray:
    """(N, T) arrays -> int32 (T, N, 4) StepInput rows, frame-major as GpuRunner.rollout wants them."""
    N, T = buttons.shape
    raw = np.zeros((T, N, 4), np.int32)
    raw[:, :, 0] = buttons.T
    raw[:, :, 1] = sx.T.astype(np.float32).view(np.int32)
    if sy is not None:
        raw[:, :, 2] = sy.T.astype(np.float32).view(np.int32)
    raw[:, :, 3] = tricks.T
    return raw


def mutate_batch(parents: np.ndarray, rate: float, sigma: float, special_index, special_sigma, rng) -> np.ndarray:
    """brute_force.mutate for a whole batch: each gene moves by N(0, sigma) with probability `rate`; the special gene
    (start boost) gets its own independent draw with special_sigma, as in the original."""
    N, G = parents.shape
    out = np.where(rng.random((N, G)) < rate, parents + rng.normal(0.0, sigma, (N, G)), parents)
    if special_index is not None and special_sigma is not None:
        g = parents[:, special_index]
        out[:, special_index] = np.where(rng.random(N) < rate, g + rng.normal(0.0, special_sigma, N), g)
    return out


# ---------------------------------------------------------------------------------------------- the search

class GpuBruteForceSearch:
    """Same interface as brute_force.BruteForceSearch (start/stop/join/status/best/best_snapshot/
    locked_prefix_frames), so brute_gui_server.py can drive either. One iteration = one generation of `batch`.

    The individual is brute_force's control-point genome (a float array). EditSearch below swaps in a different
    individual -- a real input stream -- through the hooks _initial / _children / _inputs / frames_of / cpu_result."""

    mode = "genome"

    def __init__(self, config: bf.SearchConfig, on_status: Optional[Callable[[dict], None]] = None,
                 batch: int = 2048, parents: int = 1, r_kib: Optional[int] = None, seed: Optional[int] = None,
                 verify_every: int = 0, scoring: str = "brute", tte_state_weight: float = 2.0,
                 tte_speed_gain: float = 0.05, tte_height_weight: float = 0.0,
                 tte_completion_weight: float = 0.0, repair_top: int = 0, repair_frames: int = 100,
                 repair_shifts=(-3, -2, -1, 0, 1, 2, 3)):
        if config.eval_log_path or config.surrogate_path:
            raise ValueError("eval_log_path / surrogate_path are CPU-only (see tools/gpu_brute.py's docstring)")
        self.config = config
        self.batch, self.parents, self.r_kib = batch, max(1, parents), r_kib
        self.verify_every = verify_every
        if scoring not in ("brute", "lexi", "tte"):
            raise ValueError(f"scoring must be 'brute', 'lexi' or 'tte', not {scoring!r}")
        self.scoring = scoring
        self.tte_state_weight, self.tte_speed_gain = tte_state_weight, tte_speed_gain
        self.tte_height_weight, self.tte_completion_weight = tte_height_weight, tte_completion_weight
        self._tte_ref = None
        self.repair_top, self.repair_frames, self.repair_shifts = repair_top, repair_frames, tuple(repair_shifts)
        if repair_top and scoring != "tte":
            raise ValueError("--repair-top needs --scoring tte")
        self._rng = np.random.default_rng(seed)
        self._on_status = on_status
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._status = {
            "running": False, "engine": "gpu", "mode": self.mode, "iteration": 0,
            "totalIterations": config.iterations,
            "bestFitness": None, "bestFinished": False, "bestTimeMs": None, "bestCompletion": 0, "bestSpeed": None,
            "bestProgress": None, "referenceProgress": None, "progressDelta": None, "referenceCompletion": None,
            "completionDelta": None, "bestBoostCharge": None, "bestBoostFrames": None,
            "iterationsSinceImprovement": 0, "stagnant": False, "msPerIteration": None, "etaSeconds": None,
            "evaluations": 0, "raceFramesPerSecond": None, "cpuChecks": 0, "error": None,
        }
        self._best_ind = None
        self._best_genome = None  # what best()/best_snapshot() hand out: genome list, or the individual
        self._best_result: Optional[dict] = None
        self._progress_ref = None
        self._heading_ref = None
        self._locked_prefix: Optional[list] = None
        self.gpu: Optional[GpuRunner] = None

    # --- BruteForceSearch interface
    def status(self) -> dict:
        with self._lock:
            return dict(self._status)

    def best(self):
        with self._lock:
            return self._best_genome, self._best_result

    def best_snapshot(self):
        with self._lock:
            genome, result = self._best_genome, self._best_result
            progress = self._progress_of(result) if result is not None else None
            return genome, result, progress

    def best_frames_snapshot(self):
        """(full frame list, result, progress) of the current best, read under one lock -- what an .rkg export needs
        in either mode."""
        with self._lock:
            ind, result = self._best_ind, self._best_result
            progress = self._progress_of(result) if result is not None else None
        return (self.frames_of(ind) if ind is not None else None), result, progress

    def locked_prefix_frames(self):
        return self._locked_prefix

    def start(self):
        if self._thread and self._thread.is_alive():
            raise RuntimeError("search already running")
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()

    def join(self, timeout: Optional[float] = None):
        if self._thread:
            self._thread.join(timeout)

    def _set_status(self, **kwargs):
        with self._lock:
            self._status.update(kwargs)
            snapshot = dict(self._status)
        if self._on_status:
            self._on_status(snapshot)

    def _progress_of(self, result):
        if self._progress_ref is None or result is None:
            return None
        return bf.piecewise_progress((result["posX"], result["posY"], result["posZ"]), self._progress_ref)

    def _fitness(self, result) -> float:
        cfg = self.config
        f = bf.fitness(result, cfg.fitness_metric, self._progress_ref, cfg.boost_bonus_weight,
                cfg.completion_bonus_weight, cfg.z_bonus_weight, cfg.completion_tolerance, cfg.min_boost_charge,
                cfg.speed_bonus_weight, cfg.max_boost_charge, cfg.accel_hold_bonus_weight, cfg.heading_bonus_weight)
        if self.scoring == "brute" or cfg.fitness_metric != "progress" or result["finished"]:
            return f
        if self.scoring == "tte":
            return self._tte_fitness(result)
        # "lexi" (opt-in; gpu_chain.py uses it): fitness() unchanged for candidates that pass its gates, except that
        # the polyline projection is capped at the reference line's end; candidates that fail a gate are ranked by
        # the game's own completion first and the capped projection second, instead of by the raw projection alone
        # (which has no upper bound -- a chained search exploited it: projected progress +21643 while completion fell
        # below its starting value).
        pos = (result["posX"], result["posY"], result["posZ"])
        raw, capped = bf.piecewise_progress(pos, self._progress_ref), capped_progress(pos, self._progress_ref)
        if f < GATE_PENALTY / 10:
            return f + raw - capped
        return 10 * GATE_PENALTY + (100_000 - result["completion"]) * 100_000.0 - capped

    def _tte_fitness(self, result) -> float:
        return self._tte_eval(result)[0]

    def _tte_eval(self, result):
        """"tte" (time to end) scoring: the candidate's estimated finish time in ms, from where the reference ghost
        was in the same state. Finished candidates score their real time, so the two are directly comparable, and the
        reference itself scores exactly its own time.

        The candidate at frame T is matched to the reference frame k0 nearest in state (position, speed deficit,
        heading) within +-200 frames of T; the sub-frame position along the reference's direction of travel refines
        it. Its estimated lag against the reference is
            T - k0 - (units ahead along the line / reference speed) - speed_gain * (speed - reference speed)
            + state_weight * sqrt(state distance)
        frames (plus, with --tte-height-weight, the candidate's height above/below the reference's at that frame as a
        state-distance term, which is what makes airtime the reference didn't have, or a fall, cost something), and
        the estimate is reference_time + lag * 1000/60. Unlike the polyline progress score this is in
        the units of the goal, is not tied to the reference's exact line (a different line scores by how fast the
        reference would be from there), and has no completion gate to hide small gains behind. The start-boost
        gates still apply (they are about the countdown, not the route)."""
        cfg = self.config
        for key, lo in (("min", cfg.min_boost_charge), ("max", cfg.max_boost_charge)):
            if lo is not None and "boostCharge" in result and (
                    result["boostCharge"] < lo if key == "min" else result["boostCharge"] > lo):
                return 10 * GATE_PENALTY + (100_000 - result["completion"]) * 100_000.0, None
        R = self._tte_ref
        T = int(result["frames"])
        lo, hi = max(1, T - 200), min(R["F"] - 1, T + 200)
        if hi <= lo:
            return 10 * GATE_PENALTY + (100_000 - result["completion"]) * 100_000.0, None
        sl = slice(lo, hi + 1)
        dx, dz = result["posX"] - R["x"][sl], result["posZ"] - R["z"][sl]
        qx, qy, qz, qw = result["rotX"], result["rotY"], result["rotZ"], result["rotW"]
        fx, fz = 2 * (qx * qz + qw * qy), 1 - 2 * (qx * qx + qy * qy)
        sp = float(result["speed"])
        D = ((dx * dx + dz * dz) / 300.0 ** 2 + (np.maximum(0.0, R["s"][sl] - sp) / 15.0) ** 2
             + ((fx - R["fx"][sl]) ** 2 + (fz - R["fz"][sl]) ** 2) / 0.3 ** 2)
        if self.tte_height_weight > 0:  # airtime the reference did not have (or a fall): height off its height there
            D = D + self.tte_height_weight * ((result["posY"] - R["y"][sl]) / 60.0) ** 2
        if self.tte_completion_weight > 0:  # the matched frame must be where the candidate is on the lap (~6 frames)
            D = D + self.tte_completion_weight * ((result["completion"] - R["c"][sl]) / 50.0) ** 2
        i = int(np.argmin(D))
        k0 = lo + i
        vx, vz = R["vx"][k0], R["vz"][k0]
        vl = (vx * vx + vz * vz) ** 0.5
        ahead = float(np.clip((dx[i] * vx + dz[i] * vz) / (vl * vl), -2.0, 2.0)) if vl > 1.0 else 0.0
        lag = (T - k0 - ahead - self.tte_speed_gain * (sp - float(R["s"][k0]))
               + self.tte_state_weight * float(D[i]) ** 0.5)
        return R["ms"] + lag * 1000.0 / 60.0, k0

    def _build_tte_ref(self):
        """The reference ghost frame by frame on the GPU: position, speed, forward axis and velocity per frame count,
        up to its finish."""
        cfg = self.config
        frames = bf.decode_rkg(cfg.reference_rkg)[3]
        self._ref_frames = frames
        eng = self.gpu.eng
        eng.clone_template(1, 1)
        rows = [cp.asnumpy(eng.rows(1, 1, 0))[0]]
        fin = None
        for f, fr in enumerate(frames):
            eng.step(1, 1, cp.asarray(raw_inputs([fr])))
            rows.append(cp.asnumpy(eng.rows(1, 1, f + 1))[0])
            if int(rows[-1][STAGE]) == FINISH_GLOBAL:
                fin = f + 1
                break
        if fin is None:
            raise ValueError("--scoring tte needs a reference ghost that finishes the race")
        A = np.stack(rows)
        x, z = A[:, COL["px"]], A[:, COL["pz"]]
        q = [A[:, COL[n]] for n in ("qx", "qy", "qz", "qw")]
        vx, vz = np.zeros_like(x), np.zeros_like(z)
        vx[:-1], vz[:-1] = np.diff(x), np.diff(z)
        self._tte_ref = {
            "F": fin, "ms": float(A[fin, TIMER]), "x": x, "z": z, "y": A[:, COL["py"]],
            "c": np.floor(A[:, COL["completion"]].astype(np.float32) * np.float32(10000.0)), "s": A[:, COL["speed"]], "vx": vx, "vz": vz,
            "fx": 2 * (q[0] * q[2] + q[3] * q[1]), "fz": 1 - 2 * (q[0] * q[0] + q[1] * q[1]),
        }

    # --- setup
    def prepare(self):
        """Everything before the first generation: GPU engine, prefix, references, base slot. Separate from _run so
        the parity check can use it too."""
        cfg = self.config
        self.gpu = GpuRunner(self.batch, self.r_kib, (cfg.course, cfg.character, cfg.vehicle))
        self._locked_prefix = (bf.prefix_frames_from_rkg(cfg.prefix_rkg, cfg.prefix_frame_count)
                if cfg.prefix_rkg else None)
        self._heading_ref = None
        if cfg.heading_checkpoint_frame is not None:
            if not cfg.reference_rkg:
                raise ValueError("heading_checkpoint_frame requires reference_rkg")
            self._heading_ref = bf.compute_heading_reference(cfg.binary, cfg.reference_rkg, cfg.course,
                    cfg.character, cfg.vehicle, cfg.heading_checkpoint_frame, run_many=self.gpu.run_many)
        self._progress_ref = None
        if cfg.fitness_metric == "progress":
            if not cfg.reference_rkg:
                raise ValueError("fitness_metric='progress' requires reference_rkg")
            self._progress_ref = bf.compute_progress_reference(cfg.binary, cfg.reference_rkg, cfg.course,
                    cfg.character, cfg.vehicle, cfg.progress_start_frame, cfg.max_frames,
                    run_many=self.gpu.run_many)

        if self.scoring == "tte" and cfg.fitness_metric == "progress":
            self._build_tte_ref()

        # The frames every candidate shares: the locked prefix, or the intro (always neutral, see frames_from_genome).
        self.seg_start = len(self._locked_prefix) if self._locked_prefix is not None else 0
        shared = (list(self._locked_prefix) if self._locked_prefix is not None
                else [NEUTRAL] * bf.COUNTDOWN_START_FRAME)
        self.P = min(len(shared), cfg.max_frames)
        if self.P >= cfg.max_frames:
            raise ValueError(f"nothing to search: the shared prefix ({len(shared)} frames) reaches max_frames")
        self.shared = shared[:self.P]
        # accelFramesHeld (see fitness()) over the shared frames; the tail's part is added per candidate
        self.shared_accel = sum(1 for i in range(bf.COUNTDOWN_START_FRAME, min(bf.GO_FRAME, self.P))
                if self.shared[i][0] & bf.BUTTON_ACCELERATE)
        cp_frame = self._heading_ref["checkpoint_frame"] if self._heading_ref else None
        if cp_frame is not None and cp_frame > cfg.max_frames:
            raise ValueError("heading_checkpoint_frame is past max_frames")

        eng = self.gpu.eng
        eng.clone_template(0, 1)
        base, _run, cap = self.gpu.rollout(0, 1, cp.asarray(raw_inputs(self.shared)[:, None, :]), 0,
                cp.asarray(np.array([self.P], np.int32)), capture=(cp_frame,) if cp_frame else ())
        base = cp.asnumpy(base)[0]
        if int(base[STAGE]) == FINISH_GLOBAL:
            raise ValueError("the race already finishes inside the locked prefix")
        self.base_heading = None
        if cp_frame is not None and cp_frame <= self.P:
            # inside the shared prefix: the same value for every candidate
            self.base_heading = self._heading_of(cp.asnumpy(cap[cp_frame])[0])
        self.cp_frame = cp_frame

    def _heading_of(self, row) -> float:
        r = result_from_row(row, 0)
        delta = bf._signed_heading_delta(self._heading_ref["base_front"], (r["frontX"], r["frontZ"]))
        return delta * self._heading_ref["sign"]

    # --- individuals (genome mode; EditSearch overrides these)
    def _initial(self) -> list:
        cfg = self.config
        count, _a, self._boost_idx, _t = genome_layout(cfg)
        if cfg.seed_genome is not None:
            seed = list(cfg.seed_genome[:count]) + [0.0] * max(0, count - len(cfg.seed_genome))
        else:
            seed = bf.random_genome(cfg.n_points, cfg.search_start_boost, cfg.search_accelerate, cfg.search_trick)
        return [np.asarray(seed, np.float64)]

    def _children(self, parents: list, n: int) -> np.ndarray:
        cfg = self.config
        stack = np.stack(parents)
        special_sigma = cfg.start_boost_mutation_sigma if self._boost_idx is not None else None
        return mutate_batch(stack[np.arange(n) % len(parents)], cfg.mutation_rate, cfg.mutation_sigma,
                self._boost_idx, special_sigma, self._rng)

    def _inputs(self, inds: np.ndarray):
        """Individuals -> (buttons, trick, stickX, stickY) arrays (N, max_frames - P) for frames P.."""
        b, t, sx = frames_batch(inds, self.config, self.seg_start, self.P, self.config.max_frames)
        return b, t, sx, None

    def frames_of(self, ind) -> list:
        """The full frame list (absolute frame 0..max_frames-1) an individual stands for, built the original way."""
        cfg = self.config
        return list(bf.frames_from_genome([float(x) for x in ind], cfg.step, cfg.max_frames, cfg.search_start_boost,
                cfg.search_accelerate, cfg.search_trick, cfg.n_points, self._locked_prefix, cfg.ternary_steering))

    def _best_handout(self, ind):
        return [float(x) for x in ind]

    def cpu_result(self, ind) -> dict:
        """The same genome through the CPU path (BruteForceSearch._evaluate, kinoko_host subprocesses)."""
        cpu = bf.BruteForceSearch(self.config)
        cpu._locked_prefix = self._locked_prefix
        cpu._heading_ref = self._heading_ref
        cpu._progress_ref = self._progress_ref
        return cpu._evaluate([float(x) for x in ind])

    # --- evaluation
    def check_frames(self, ind) -> None:
        """The vectorized inputs must equal frames_of()'s (the original construction), bit for bit."""
        want = raw_inputs(self.frames_of(ind))
        b, t, sx, sy = self._inputs(np.asarray(ind)[None])
        got = np.concatenate([raw_inputs(self.shared), pack_inputs(b, t, sx, sy)[:, 0]])
        if not np.array_equal(got, want):
            bad = int(np.nonzero((got != want).any(1))[0][0])
            raise AssertionError(f"vectorized frames differ from the original at frame {bad}: "
                                 f"{got[bad].tolist()} vs {want[bad].tolist()}")

    def evaluate(self, inds: np.ndarray) -> list:
        """Individuals (N, ...) -> result dicts, each what BruteForceSearch._evaluate returns for those frames."""
        cfg = self.config
        results = []
        for k in range(0, len(inds), self.batch):
            g = inds[k:k + self.batch]
            n = len(g)
            buttons, tricks, sx, sy = self._inputs(g)
            self.gpu.eng.copy([0] * n, list(range(1, n + 1)))
            ends = cp.full(n, cfg.max_frames, cp.int32)
            capture = (self.cp_frame,) if self.cp_frame is not None and self.cp_frame > self.P else ()
            final, run, cap = self.gpu.rollout(1, n, cp.asarray(pack_inputs(buttons, tricks, sx, sy)), self.P, ends,
                    capture)
            final, run = cp.asnumpy(final), cp.asnumpy(run)
            cap_rows = cp.asnumpy(cap[self.cp_frame]) if capture else None
            lo, hi = self.P, min(bf.GO_FRAME, cfg.max_frames)
            accel = self.shared_accel + ((buttons[:, :max(0, hi - lo)] & bf.BUTTON_ACCELERATE) != 0).sum(1)
            for j in range(n):
                r = result_from_row(final[j], run[j])
                r["accelFramesHeld"] = int(accel[j])
                if self._heading_ref is not None:
                    r["headingProgress"] = (self.base_heading if cap_rows is None else self._heading_of(cap_rows[j]))
                results.append(r)
        return results

    def cpu_result_frames(self, frames) -> dict:
        """Any frame list through kinoko_host, post-processed the way BruteForceSearch._evaluate does it
        (accelFramesHeld, and headingProgress from a second run cut at the checkpoint)."""
        cfg = self.config
        frames = list(frames)[:cfg.max_frames]
        r = bf.run_kinoko(cfg.binary, bf.encode_task(cfg.course, cfg.character, cfg.vehicle, frames, cfg.max_frames))
        r["accelFramesHeld"] = sum(1 for i in range(bf.COUNTDOWN_START_FRAME, min(bf.GO_FRAME, cfg.max_frames))
                if i < len(frames) and frames[i][0] & bf.BUTTON_ACCELERATE)
        if self._heading_ref is not None:
            f = self._heading_ref["checkpoint_frame"]
            c = bf.run_kinoko(cfg.binary, bf.encode_task(cfg.course, cfg.character, cfg.vehicle, frames[:f], f))
            r["headingProgress"] = bf._signed_heading_delta(self._heading_ref["base_front"],
                    (c["frontX"], c["frontZ"])) * self._heading_ref["sign"]
        return r

    # --- the loop
    def _run(self):
        cfg = self.config
        self._set_status(running=True, error=None)
        try:
            self.prepare()
            ref_progress = self._progress_ref["total_length"] if self._progress_ref else None
            ref_completion = self._progress_ref["reference_completion"] if self._progress_ref else None

            t0 = time.time()
            init = self._initial()
            for ind in init:
                self.check_frames(ind)
            init_arr = np.stack(init)
            pop = sorted(self._rank(init_arr, self.evaluate(init_arr), top=len(init_arr)),
                    key=lambda s: s[0])[:self.parents]  # best first
            best_fit = pop[0][0]
            evaluations = len(init)
            since = 0

            def publish(i, **extra):
                fit, ind, result = pop[0]
                with self._lock:
                    self._best_ind, self._best_genome, self._best_result = ind, self._best_handout(ind), result
                prog = self._progress_of(result)
                boost_frames = bf.start_boost_tier(result["boostCharge"])[0]
                el = time.time() - t0
                ms = el / max(i, 1) * 1000.0
                self._set_status(iteration=i, bestFitness=fit, bestFinished=result["finished"],
                        bestTimeMs=result["timeMs"] if result["finished"] else None,
                        bestCompletion=result["completion"], bestSpeed=result["speed"], bestProgress=prog,
                        referenceProgress=ref_progress,
                        progressDelta=prog - ref_progress if prog is not None and ref_progress is not None else None,
                        referenceCompletion=ref_completion,
                        completionDelta=(result["completion"] - ref_completion if ref_completion is not None
                                else None),
                        bestBoostCharge=result["boostCharge"], bestBoostFrames=boost_frames,
                        iterationsSinceImprovement=since, msPerIteration=ms,
                        etaSeconds=ms * (cfg.iterations - i) / 1000.0, evaluations=evaluations,
                        raceFramesPerSecond=self.gpu.race_frames / max(el, 1e-9), **extra)

            publish(0)
            for i in range(1, cfg.iterations + 1):
                if self._stop.is_set():
                    break
                children = self._children([p[1] for p in pop], self.batch)
                self.check_frames(children[self._rng.integers(len(children))])
                results = self.evaluate(children)
                evaluations += len(children)
                scored = pop + self._rank(children, results)
                scored.sort(key=lambda s: s[0])  # stable: a parent wins ties, as in the CPU's strict `fit < best`
                pop, seen = [], set()
                for s in scored:
                    key = s[1].tobytes()
                    if key not in seen:
                        seen.add(key)
                        pop.append(s)
                    if len(pop) >= self.parents:
                        break
                if pop[0][0] < best_fit:
                    best_fit = pop[0][0]
                    since = 0
                else:
                    since += 1
                stagnant = cfg.stagnation_stop_after is not None and since >= cfg.stagnation_stop_after
                extra = {"stagnant": stagnant}
                if self.verify_every and i % self.verify_every == 0:
                    self._verify(pop[0])
                    extra["cpuChecks"] = self._status["cpuChecks"] + 1
                publish(i, **extra)
                if stagnant:
                    break
        except Exception as exc:  # noqa: BLE001 -- surface to the GUI/CLI, same as BruteForceSearch
            import traceback
            traceback.print_exc()
            self._set_status(error=str(exc))
        finally:
            # give the card back so an idle GUI doesn't hold it: the race slots (~1.2 GB at batch 2048) and the
            # per-thread stack reservation GpuRace sets up (32 KiB x every resident thread, ~1.6 GB); the next
            # GpuRace sets the stack limit again
            self.gpu = None
            cp.get_default_memory_pool().free_all_blocks()
            cp.cuda.runtime.deviceSetLimit(0, 1024)  # cudaLimitStackSize
            self._set_status(running=False)

    def _rank(self, inds, results, top=None):
        """[(fitness, individual, result)] for the candidates that compete. Without --repair-top: every candidate, scored
        as always. With it: only the best `top` by that cheap score, re-scored after a repair continuation (see
        _repair), so every fitness in the population is a repaired one."""
        scored = [(self._fitness(r), inds[j], r) for j, r in enumerate(results)]
        if not self.repair_top:
            return scored
        scored.sort(key=lambda s: s[0])
        keep = scored[:top or self.repair_top]
        rep = self._repair([k[1] for k in keep], [k[2] for k in keep])
        return [(f, k[1], k[2]) for f, k in zip(rep, keep)]

    def _repair(self, inds, results):
        """Fitness of each candidate after a repair continuation. A candidate is a state at the scoring frame T whose
        later inputs (the base run's) were tuned for a different state, so a window can win by landing in a drift phase
        the rest of the run cannot use. Here every unfinished candidate is re-simulated to T and branched into one race
        per time shift of the REFERENCE ghost's inputs, starting from the reference frame it matches best (its k0, as
        in --scoring tte) and run --repair-frames further; its fitness is the best tte score of those branches at
        T + --repair-frames. A candidate whose state the reference's own inputs cannot continue scores badly."""
        cfg = self.config
        fit = [self._fitness(r) for r in results]
        live = [j for j, r in enumerate(results) if not r["finished"] and self._tte_eval(r)[1] is not None]
        V, Rf = len(self.repair_shifts), self.repair_frames
        if not live:
            return fit
        if len(live) * (V + 1) + 1 > self.batch:
            raise ValueError("--repair-top x (shifts + 1) does not fit in --batch")
        n = len(live)
        buttons, tricks, sx, sy = self._inputs(np.stack([inds[j] for j in live]))
        eng = self.gpu.eng
        eng.copy([0] * n, list(range(1, n + 1)))
        final, _run, _cap = self.gpu.rollout(1, n, cp.asarray(pack_inputs(buttons, tricks, sx, sy)), self.P,
                                             cp.full(n, cfg.max_frames, cp.int32))
        ref = self._ref_frames
        first = n + 1
        src, branch = [], np.zeros((Rf, n * V, 4), np.int32)
        for jj, j in enumerate(live):
            k0 = self._tte_eval(results[j])[1]
            for v, sh in enumerate(self.repair_shifts):
                src.append(1 + jj)
                idx = [min(max(k0 + sh + t, 0), len(ref) - 1) for t in range(Rf)]
                branch[:, jj * V + v, :] = raw_inputs([ref[i] for i in idx])
        eng.copy(src, list(range(first, first + n * V)))
        final2, run2, _cap2 = self.gpu.rollout(first, n * V, cp.asarray(branch), cfg.max_frames,
                                               cp.full(n * V, cfg.max_frames + Rf, cp.int32))
        rows2, run2 = cp.asnumpy(final2), cp.asnumpy(run2)
        for jj, j in enumerate(live):
            best = None
            for v in range(V):
                r = result_from_row(rows2[jj * V + v], run2[jj * V + v])
                f = self._fitness(r)
                best = f if best is None else min(best, f)
            fit[j] = best
        return fit

    def _verify(self, entry):
        """Re-evaluate one individual on the CPU; any difference in its result or fitness is a GPU port bug."""
        fit, ind, gpu = entry
        cpu = self.cpu_result(ind)
        diffs = {k: (gpu[k], cpu.get(k)) for k in gpu if k != "frames" and gpu[k] != cpu.get(k)}
        cpu_fit = self._fitness(cpu)
        if diffs or (cpu_fit != fit and not self.repair_top):  # with repair, fit is a branched score the CPU path lacks
            raise RuntimeError(f"GPU/CPU mismatch on the current best: fitness {fit} vs {cpu_fit}, fields {diffs}")


# ---------------------------------------------------------------------------------------------- edit mode

class EditSearch(GpuBruteForceSearch):
    """Searches a real input stream instead of the control-point genome: the individual is the tail (frames P..
    max_frames-1) of a base ghost's inputs -- buttons, trick direction, stickX, stickY, each frame -- and children
    are that stream with a few local edits. Everything the genome can't express is reachable this way: hop/drift
    (buttons 0x8 / 0x2), any trick direction (1-4), stickY, and retiming an existing input by a frame or two.

    Edits (each child gets 1 + Poisson(edits_per_child - 1) of them, all inside edit_window):
        stick_x   a 1-12 frame run of stickX nudged by +-1/2 grid steps or set to a value (full lock / neutral / any)
        stick_y   the same for stickY
        edge      a button or trick change moved earlier/later by 1-2 frames (e.g. a drift start, a ramp trick)
        block     all inputs of a 5-60 frame block shifted one frame earlier or later
        trick     a trick direction 1-4 set for 1-2 frames, or an existing trick cleared
        button    hop, brake, hop+brake (drift) or accelerate turned on/off over a 1-12 frame run
    Values stay on the .rkg grid (stick 0-14, buttons 4 bits, trick 0-4) so every candidate exports exactly.

    Drift-structure edits (drift_ops = the fraction of edits drawn from these instead of the ones above; a drift is
    the hop button 0x8 held, with brake 0x2 and accelerate 0x1 -- buttons 11 -- and releasing it fires the mini-turbo):
        d_start   a drift's start moved 1-8 frames earlier/later (30% of the time up to 20)
        d_end     a drift's release (the mini-turbo) moved 1-10 frames earlier/later (30% of the time up to 40)
        d_gap     a drift cut in two by a 1-4 frame release, or two drifts closer than 10 frames merged
        d_new     a new drift of 20-150 frames, steering hard into it for its first 5-25 frames
        d_flip    the stick mirrored across 10-100 frames of a drift (the other drift direction)
        d_snake   10-120 frames of a drift's stick re-patterned as a period 2-12 hi/lo alternation (charges faster)
        d_scale   a drift's steering softened or sharpened (stick deviation x0.5-1.25) over 20-150 frames
        d_item    an item (mushroom) press moved 1-30 frames earlier/later
        d_delete  1-6 frames removed at a point, every later input (to the end of the tail) shifted that much earlier
        d_insert  1-6 frames inserted at a point (the frame there repeated), every later input shifted later

    Graft edits (--graft-rkg: other ghosts whose inputs may be copied in; they need no finish and need not be from the
    same run -- e.g. a community attempt's better lap 1):
        g_copy    10-300 frames of a graft ghost copied over the same frames of the candidate (source shifted -10..10)
        g_drift   one whole drift of a graft ghost (its hop-button run, plus 0-15 frames either side) copied in
        g_item    the candidate's item (mushroom) press moved to where a graft ghost presses it (+-3 frames)"""

    mode = "edits"
    OPS = ("stick_x", "stick_y", "edge", "block", "trick", "button")
    OP_WEIGHTS = np.array([0.30, 0.15, 0.20, 0.10, 0.10, 0.15])
    DRIFT_OPS = ("d_start", "d_end", "d_gap", "d_new", "d_flip", "d_snake", "d_scale", "d_item", "d_delete",
                 "d_insert")
    DRIFT_WEIGHTS = np.array([0.15, 0.15, 0.08, 0.08, 0.08, 0.12, 0.08, 0.08, 0.09, 0.09])
    DRIFT_MASK = 0xA  # hop + brake
    GRAFT_OPS = ("g_copy", "g_drift", "g_item")
    GRAFT_WEIGHTS = np.array([0.4, 0.45, 0.15])

    def __init__(self, config: bf.SearchConfig, base_rkg: str, base_shifts=(0,), edits_per_child: float = 2.0,
                 edit_window=None, extra_bases=(), drift_ops: float = 0.0, graft_rkgs=(), graft_ops: float = 0.0, **kw):
        super().__init__(config, **kw)
        self.drift_ops = drift_ops
        self.graft_rkgs, self.graft_ops = list(graft_rkgs), (graft_ops if graft_rkgs else 0.0)
        self._graft = []
        self.base_rkg, self.base_shifts = base_rkg, tuple(base_shifts)
        self.extra_bases = list(extra_bases)  # more starting streams: full frame lists indexed by absolute frame
        self.edits_per_child = edits_per_child
        self.edit_window = edit_window

    def _initial(self) -> list:
        cfg = self.config
        frames = bf.decode_rkg(self.base_rkg)[3]
        lo, hi = self.edit_window or (self.P, cfg.max_frames)
        self.w0, self.w1 = max(0, lo - self.P), min(cfg.max_frames, hi) - self.P
        if self.w1 - self.w0 < 2:
            raise ValueError(f"edit window {lo}-{hi} is outside the searched frames {self.P}-{cfg.max_frames}")
        out = []
        # graft sources as int16 (4, frames + 1) streams indexed by absolute frame; past a source's recording:
        # accelerate held, stick neutral
        self._graft = []
        for path in self.graft_rkgs:
            fr = bf.decode_rkg(path)[3]
            self._graft.append(np.array([[f[0] for f in fr] + [1], [f[1] for f in fr] + [0],
                                         [bf._stick_raw(f[2]) for f in fr] + [7],
                                         [bf._stick_raw(f[3]) for f in fr] + [7]], np.int16))
        # past the end of a base's recording: accelerate held, stick neutral (neutral input would coast, and an edit
        # can only switch accelerate on for a few frames at a time)
        pad = (bf.BUTTON_ACCELERATE, 0, 0.0, 0.0)
        tails = [[frames[i + s] if 0 <= i + s < len(frames) else pad for i in range(self.P, cfg.max_frames)]
                 for s in self.base_shifts]
        tails += [[fr[i] if i < len(fr) else pad for i in range(self.P, cfg.max_frames)] for fr in self.extra_bases]
        for tail in tails:
            ind = np.array([[f[0] for f in tail], [f[1] for f in tail],
                            [bf._stick_raw(f[2]) for f in tail], [bf._stick_raw(f[3]) for f in tail]], np.int16)
            out.append(ind)
        return out

    def _inputs(self, inds: np.ndarray):
        inds = inds.astype(np.int32)
        return inds[:, 0], inds[:, 1], (inds[:, 2] - 7) / 7.0, (inds[:, 3] - 7) / 7.0

    def frames_of(self, ind) -> list:
        tail = [(int(ind[0, i]), int(ind[1, i]), (int(ind[2, i]) - 7) / 7.0, (int(ind[3, i]) - 7) / 7.0)
                for i in range(ind.shape[1])]
        return list(self.shared) + tail

    def _best_handout(self, ind):
        return ind

    def cpu_result(self, ind) -> dict:
        return self.cpu_result_frames(self.frames_of(ind))

    def _children(self, parents: list, n: int) -> np.ndarray:
        rng = self._rng
        out = np.empty((n,) + parents[0].shape, np.int16)
        ops = rng.choice(len(self.OPS), size=(n, 16), p=self.OP_WEIGHTS / self.OP_WEIGHTS.sum())
        counts = 1 + rng.poisson(max(0.0, self.edits_per_child - 1.0), n)
        use_drift = rng.random((n, 16)) < self.drift_ops
        dops = rng.choice(len(self.DRIFT_OPS), size=(n, 16), p=self.DRIFT_WEIGHTS / self.DRIFT_WEIGHTS.sum())
        use_graft = rng.random((n, 16)) < self.graft_ops
        gops = rng.choice(len(self.GRAFT_OPS), size=(n, 16), p=self.GRAFT_WEIGHTS / self.GRAFT_WEIGHTS.sum())
        for j in range(n):
            c = parents[j % len(parents)].copy()
            for k in range(min(int(counts[j]), 16)):
                if use_graft[j, k]:
                    self._edit(c, self.GRAFT_OPS[gops[j, k]])
                else:
                    self._edit(c, self.DRIFT_OPS[dops[j, k]] if use_drift[j, k] else self.OPS[ops[j, k]])
            out[j] = c
        return out

    def _drift_runs(self, c):
        """[(start, end)) of every stretch of the edit window with the hop/drift button held."""
        h = np.concatenate(([0], (c[0, self.w0:self.w1] & 8) != 0, [0])).astype(np.int8)
        d = np.diff(h)
        return list(zip((np.nonzero(d == 1)[0] + self.w0).tolist(), (np.nonzero(d == -1)[0] + self.w0).tolist()))

    def _drift_edit(self, c, op):
        rng, w0, w1, DM = self._rng, self.w0, self.w1, self.DRIFT_MASK
        runs = self._drift_runs(c)
        if op in ("d_delete", "d_insert"):
            k = min(int(rng.geometric(0.4)), 6)
            a = int(rng.integers(w0 + 1, max(w0 + 2, w1 - 1)))
            if op == "d_delete":
                c[:, a:-k] = c[:, a + k:].copy()
                c[:, -k:] = np.array([[1], [0], [7], [7]], c.dtype)  # past the recording: accelerate, stick neutral
            else:
                c[:, a + k:] = c[:, a:-k].copy()
                c[:, a:a + k] = c[:, a - 1:a]
            return None
        if op == "d_item":
            items = np.nonzero(c[0, w0:w1] & 4)[0] + w0
            if len(items) == 0:
                return self._edit(c, "stick_x")
            i = int(rng.choice(items))
            j = int(np.clip(i + rng.choice([-1, 1]) * int(rng.integers(1, 31)), w0, w1 - 1))
            c[0, i] &= ~4 & 0xF
            c[0, j] |= 4
            return None
        if op == "d_new" or not runs:
            free = np.nonzero((c[0, w0:w1] & 8) == 0)[0] + w0
            if len(free) == 0:
                return self._edit(c, "stick_x")
            a = int(rng.choice(free))
            b = min(a + int(rng.integers(20, 151)), w1)
            c[0, a:b] |= DM | 1
            k = int(rng.integers(5, 26))
            c[2, a:min(a + k, b)] = int(rng.choice([0, 14]))
            return None
        s, e = runs[int(rng.integers(0, len(runs)))]
        if op == "d_start":
            d = int(rng.choice([-1, 1])) * int(rng.integers(1, 21 if rng.random() < 0.3 else 9))
            if d < 0:
                c[0, max(s + d, w0):s] |= DM | 1
            else:
                c[0, s:min(s + d, e)] &= ~DM & 0xF
        elif op == "d_end":
            d = int(rng.choice([-1, 1])) * int(rng.integers(1, 41 if rng.random() < 0.3 else 11))
            if d > 0:
                c[0, e:min(e + d, w1)] |= DM | 1
            else:
                c[0, max(e + d, s):e] &= ~DM & 0xF
        elif op == "d_gap":
            near = [(a, b) for (a, b) in zip(runs, runs[1:]) if b[0] - a[1] <= 10]
            if near and rng.random() < 0.5:
                (a0, a1), (b0, b1) = near[int(rng.integers(0, len(near)))]
                c[0, a1:b0] |= DM | 1
            elif e - s > 20:
                g = int(rng.integers(s + 8, e - 8))
                c[0, g:g + int(rng.integers(1, 5))] &= ~DM & 0xF
        else:  # d_flip / d_snake / d_scale act on part of the run
            lo, hi = {"d_flip": (10, 100), "d_snake": (10, 120), "d_scale": (20, 150)}[op]
            n = min(int(rng.integers(lo, hi + 1)), e - s)
            x = int(rng.integers(s, e - n + 1))
            y = x + n
            if op == "d_flip":
                c[2, x:y] = 14 - c[2, x:y]
            elif op == "d_scale":
                k = float(rng.choice([0.5, 0.7, 0.85, 1.2, 1.25]))
                c[2, x:y] = np.clip(7 + np.round(k * (c[2, x:y].astype(np.float64) - 7)), 0, 14).astype(np.int16)
            else:
                sign = 1 if c[2, x:y].astype(np.int32).mean() >= 7 else -1
                hi_v = 7 + sign * int(rng.choice([7, 6, 5, 3]))
                lo_v = 7 + sign * int(rng.choice([7, 3, 1, 0, -3]))
                p = int(rng.integers(2, 13))
                duty = max(1, int(round(p * rng.uniform(0.3, 0.8))))
                ph = int(rng.integers(0, p))
                c[2, x:y] = np.where(((np.arange(n) + ph) % p) < duty, hi_v, lo_v)
        return None

    def _graft_edit(self, c, op):
        rng, w0, w1 = self._rng, self.w0, self.w1
        if not self._graft:
            return self._edit(c, "stick_x")
        src = self._graft[int(rng.integers(0, len(self._graft)))]
        last = src.shape[1] - 1
        P = self.P

        def take(a, b, sh):
            """source frames for candidate indices a..b-1, shifted by sh, from absolute frame P + index"""
            idx = np.clip(np.arange(a, b) + P + sh, 0, last)
            return src[:, idx]

        if op == "g_copy":
            L = int(np.clip(round(10 * 30 ** rng.random()), 10, min(300, w1 - w0)))   # 10..300, log-uniform
            a = int(rng.integers(w0, max(w0 + 1, w1 - L + 1)))
            b = min(a + L, w1)
            c[:, a:b] = take(a, b, int(rng.integers(-10, 11)))
        elif op == "g_drift":
            sidx = np.arange(w0, w1) + P
            h = (src[0, np.clip(sidx, 0, last)] & 8) != 0
            d = np.diff(np.concatenate(([0], h.astype(np.int8), [0])))
            runs = list(zip((np.nonzero(d == 1)[0] + w0).tolist(), (np.nonzero(d == -1)[0] + w0).tolist()))
            runs = [r for r in runs if r[1] - r[0] >= 8]
            if not runs:
                return self._edit(c, "g_copy")
            s0, e0 = runs[int(rng.integers(0, len(runs)))]
            a = max(w0, s0 - int(rng.integers(0, 16)))
            b = min(w1, e0 + int(rng.integers(0, 16)))
            c[:, a:b] = take(a, b, 0)
        else:  # g_item
            sp = np.nonzero(src[0, np.clip(np.arange(w0, w1) + P, 0, last)] & 4)[0] + w0
            mine = np.nonzero(c[0, w0:w1] & 4)[0] + w0
            if len(sp) == 0 or len(mine) == 0:
                return self._edit(c, "g_copy")
            j = int(np.clip(int(sp[0]) + int(rng.integers(-3, 4)), w0, w1 - 1))
            c[0, int(mine[0])] &= ~4 & 0xF
            c[0, j] |= 4
        return None

    def _edit(self, c, op):
        if op.startswith("g_"):
            return self._graft_edit(c, op)
        if op.startswith("d_"):
            return self._drift_edit(c, op)
        rng, w0, w1 = self._rng, self.w0, self.w1
        a = int(rng.integers(w0, w1))
        run = min(int(rng.geometric(0.3)), 12)
        b = min(a + run, w1)
        if op == "stick_x" or op == "stick_y":
            row = c[2] if op == "stick_x" else c[3]
            u = rng.random()
            if u < 0.5:
                row[a:b] = np.clip(row[a:b] + rng.choice([-2, -1, 1, 2]), 0, 14)
            elif u < 0.8:
                row[a:b] = rng.choice([0, 7, 14])
            else:
                row[a:b] = rng.integers(0, 15)
        elif op == "edge":
            ch = c[int(rng.integers(0, 2))]  # buttons or trick
            edges = np.nonzero(ch[w0 + 1:w1] != ch[w0:w1 - 1])[0] + w0 + 1
            if len(edges) == 0:
                return self._edit(c, "stick_x")
            e = int(rng.choice(edges))
            d = int(rng.choice([-2, -1, 1, 2]))
            if d > 0:
                ch[e:min(e + d, w1)] = ch[e - 1]       # the new value starts later
            else:
                ch[max(e + d, w0):e] = ch[e]           # the new value starts earlier
        elif op == "block":
            length = int(rng.integers(5, 61))
            a = int(rng.integers(w0 + 1, max(w0 + 2, w1 - 1)))
            b = min(a + length, w1 - 1)
            if rng.random() < 0.5:
                c[:, a:b] = c[:, a - 1:b - 1].copy()   # one frame later
            else:
                c[:, a:b] = c[:, a + 1:b + 1].copy()   # one frame earlier
        elif op == "trick":
            nz = np.nonzero(c[1, w0:w1])[0] + w0
            if len(nz) and rng.random() < 0.3:
                i = int(rng.choice(nz))
                c[1, i] = 0
            else:
                c[1, a:min(a + int(rng.integers(1, 3)), w1)] = rng.integers(1, 5)
        elif op == "button":
            bit = int(rng.choice([0x8, 0x2, 0xA, 0x1], p=[0.35, 0.2, 0.35, 0.1]))
            if rng.random() < 0.5:
                c[0, a:b] |= bit
            else:
                c[0, a:b] &= ~bit & 0xF


def make_search(config: bf.SearchConfig, edit_base_rkg: Optional[str] = None, edit_base_shifts=(0,),
                edits_per_child: float = 2.0, edit_window=None, **kw) -> GpuBruteForceSearch:
    """Genome mode, or edit mode when edit_base_rkg is given (what the CLI and the GUI both call)."""
    if edit_base_rkg:
        return EditSearch(config, edit_base_rkg, edit_base_shifts, edits_per_child, edit_window, **kw)
    return GpuBruteForceSearch(config, **kw)


# ---------------------------------------------------------------------------------------------- CLI

def check_parity(search: GpuBruteForceSearch, count: int) -> int:
    """Evaluate `count` individuals (the starting ones, then mutations of them) on both engines and compare every
    result field and the fitness. Returns the number of mismatching individuals."""
    search.prepare()
    inds = list(search._initial())
    parents = list(inds)
    while len(inds) < count:
        if search.mode == "genome" and len(inds) % 3 == 0:
            cfg = search.config
            inds.append(np.asarray(bf.random_genome(cfg.n_points, cfg.search_start_boost, cfg.search_accelerate,
                                                    cfg.search_trick), np.float64))
        else:
            inds.append(search._children(parents, 1)[0])
            parents.append(inds[-1])  # mutations of mutations: drift further from the start
    G = np.stack(inds[:count])
    for g in G:
        search.check_frames(g)
    t = time.time()
    gpu = search.evaluate(G)
    t_gpu = time.time() - t
    from concurrent.futures import ThreadPoolExecutor
    t = time.time()
    with ThreadPoolExecutor(max_workers=8) as ex:
        cpu = list(ex.map(search.cpu_result, list(G)))
    t_cpu = time.time() - t
    bad = 0
    for j, (g, c) in enumerate(zip(gpu, cpu)):
        diffs = {k: (g[k], c.get(k)) for k in g if g[k] != c.get(k)}
        fg, fc = search._fitness(g), search._fitness(c)
        status = "OK " if not diffs and fg == fc else "BAD"
        bad += status == "BAD"
        print(f"  {status} #{j:3d}: fitness gpu {fg:.6f} cpu {fc:.6f}  finished={g['finished']} "
              f"frames={g['frames']} completion={g['completion']} speed={g['speed']:.1f} inTrick={g['inTrick']}"
              + (f"  DIFF {diffs}" if diffs else ""))
    print(f"parity: {len(G) - bad}/{len(G)} identical (GPU {t_gpu:.1f}s, CPU {t_cpu:.1f}s with 8 workers)")
    return bad


def main():
    parser = bf.build_parser(__doc__)
    for action in parser._actions:
        if action.dest == "binary":
            action.required = False
            action.default = CPU_BINARY
            action.help = "kinoko_host used for the CPU cross-checks (default: build-gpu-spike's)"
    g = parser.add_argument_group("GPU")
    g.add_argument("--batch", type=int, default=2048, help="children per generation (GPU slots, ~0.57 MiB each)")
    g.add_argument("--parents", type=int, default=1,
                   help="1 = the CPU's hill climb (mutate the best, keep it unless a child is strictly better); "
                        "k > 1 keeps the best k distinct individuals")
    g.add_argument("--r-kib", type=int, default=None,
                   help="private heap per race slot; default: the course's measured size (gpu_spike/slot_sizes.json)")
    g.add_argument("--seed", type=int, default=None, help="RNG seed for mutations and random genomes")
    g.add_argument("--verify-every", type=int, default=0,
                   help="every N generations, re-run the current best on the CPU and stop on any difference")
    g.add_argument("--tte-state-weight", type=float, default=2.0,
                   help="--scoring tte: frames of penalty per unit of state distance (position/300, speed deficit/15, "
                        "heading/0.3 rad) from the nearest reference frame")
    g.add_argument("--tte-speed-gain", type=float, default=0.05,
                   help="--scoring tte: frames credited per unit of speed above the reference's at the matched frame")
    g.add_argument("--tte-height-weight", type=float, default=0.0,
                   help="--scoring tte: weight of (height - reference height at the matched frame)/60 in the state "
                        "distance. Airtime the reference didn't have, or a fall below its height, costs; the same "
                        "jumps as the reference cost nothing. 0 = off, ~1 typical")
    g.add_argument("--repair-top", type=int, default=0, metavar="K",
                   help="--scoring tte: re-score the best K children of each generation after a repair continuation "
                        "(they are branched into one race per --repair-shifts shift of the reference ghost's inputs from "
                        "their best-matching frame and run --repair-frames further; the best branch's tte score is "
                        "their fitness). 0 = off; ~32 typical. Needs K x (shifts + 1) + 1 <= --batch")
    g.add_argument("--repair-frames", type=int, default=100, help="frames of repair continuation")
    g.add_argument("--repair-shifts", default="-3,-2,-1,0,1,2,3",
                   help="comma list of frame shifts of the reference's inputs tried by the repair continuation")
    g.add_argument("--tte-completion-weight", type=float, default=0.0,
                   help="--scoring tte: weight of (completion - reference completion at the matched frame)/50 "
                        "(0.005 of a lap) in the state distance: stops a candidate far from the reference matching a "
                        "frame on a different part of the lap. 0 = off, ~1 typical")
    g.add_argument("--scoring", choices=["brute", "lexi", "tte"], default="brute",
                   help="brute (default): fitness() as is; lexi: gate-failing candidates ranked by completion first, "
                        "progress projection capped at the reference line's end (see GpuBruteForceSearch._fitness)")
    g.add_argument("--check-parity", type=int, default=0, metavar="N",
                   help="don't search: evaluate N individuals on both GPU and CPU and compare them field by field")
    e = parser.add_argument_group("edit mode (search a real input stream instead of the genome)")
    e.add_argument("--edit-base-rkg", help="ghost whose inputs (after the prefix) are the starting individual; "
                                           "turns edit mode on (the genome flags are then ignored)")
    e.add_argument("--edit-base-shifts", default="0",
                   help="comma list: start individuals = base frames shifted by these amounts (base[i + s] at frame "
                        "i); e.g. -1,0,1 with --parents 3")
    e.add_argument("--edits-per-child", type=float, default=2.0, help="mean number of edits per child (>= 1)")
    e.add_argument("--edit-window", help="FROM-TO absolute frames edits may touch (default: the whole tail)")
    e.add_argument("--graft-rkg", nargs="+", default=[], metavar="RKG",
                   help="ghosts whose inputs the graft edits may copy in (g_copy / g_drift / g_item); they need no finish")
    e.add_argument("--graft-ops", type=float, default=0.0, metavar="FRACTION",
                   help="fraction of edits drawn from the graft edits (needs --graft-rkg); ~0.3 typical")
    e.add_argument("--drift-ops", type=float, default=0.0, metavar="FRACTION",
                   help="fraction of edits drawn from the drift-structure ops (drift start/end/split/new/flip, stick "
                        "re-patterning, steering scale, item timing); 0 = off, ~0.5 typical")
    args = parser.parse_args()
    if args.seed is not None:
        random.seed(args.seed)
    cfg = bf.config_from_args(args)

    def report(status):
        if status["bestFitness"] is None:
            return
        if status["iteration"] % 10 == 0 or not status["running"]:
            if status["bestFinished"]:
                best = "%.0fms" % status["bestTimeMs"]
            elif cfg.fitness_metric == "speed":
                best = "%.3f speed" % status["bestSpeed"]
            elif cfg.fitness_metric == "progress":
                best = "%.2f units of progress (ref %.2f), completion %+d" % (
                    status["bestProgress"], status["referenceProgress"], status["completionDelta"])
            else:
                best = str(status["bestCompletion"]) + "/10000-per-lap completion"
            pace = ""
            if status["msPerIteration"] is not None:
                pace = (f" ({status['msPerIteration']:.0f} ms/gen, {status['evaluations']:,} evals, "
                        f"{status['raceFramesPerSecond'] / 1e6:.2f}M race-frames/s)")
            print(f"gen {status['iteration']}/{status['totalIterations']} fitness={status['bestFitness']:.4f} "
                  f"best={best} speed={status['bestSpeed']}{pace}", flush=True)

    window = tuple(int(x) for x in args.edit_window.split("-")) if args.edit_window else None
    search = make_search(cfg, args.edit_base_rkg, [int(s) for s in args.edit_base_shifts.split(",")],
                         args.edits_per_child, window, on_status=report, batch=args.batch, parents=args.parents,
                         r_kib=args.r_kib, seed=args.seed, verify_every=args.verify_every,
                         scoring=args.scoring, tte_state_weight=args.tte_state_weight,
                         tte_speed_gain=args.tte_speed_gain, tte_height_weight=args.tte_height_weight,
                         tte_completion_weight=args.tte_completion_weight, repair_top=args.repair_top,
                         repair_frames=args.repair_frames,
                         repair_shifts=[int(x) for x in args.repair_shifts.split(",")], **({"drift_ops": args.drift_ops, "graft_rkgs": args.graft_rkg, "graft_ops": args.graft_ops}
                            if args.edit_base_rkg else {}))
    if args.check_parity:
        return 1 if check_parity(search, args.check_parity) else 0

    search.start()
    try:
        search.join()
    except KeyboardInterrupt:
        search.stop()
        search.join()

    frames, result, _progress = search.best_frames_snapshot()
    status = search.status()
    if status["error"]:
        print(f"error: {status['error']}", file=sys.stderr)
    if frames is None:
        sys.exit(1)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(bf.encode_rkg(cfg.course, cfg.character, cfg.vehicle, frames,
                result["timeMs"] if result["finished"] else 0))
    print(f"wrote {args.out} ({'finished ' + str(result['timeMs']) + 'ms' if result['finished'] else 'did not finish'})")

    # independent check: the WRITTEN file, decoded again and run through the CPU path, field by field and by fitness
    written = bf.decode_rkg(args.out)[3]
    cpu = search.cpu_result_frames(written)
    diffs = {k: (result[k], cpu.get(k)) for k in result if result[k] != cpu.get(k)}
    fg, fc = search._fitness(result), search._fitness(cpu)
    print(f"CPU replay of the written .rkg: {'MATCH' if not diffs and fg == fc else 'MISMATCH'} "
          f"(fitness gpu {fg:.6f} cpu {fc:.6f}){'  ' + str(diffs) if diffs else ''}")
    return 0 if not diffs and fg == fc else 2


if __name__ == "__main__":
    sys.exit(main())
