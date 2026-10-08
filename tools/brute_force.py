#!/usr/bin/env python3
"""TAS ghost brute-forcer driving the native `kinoko_host brute` binary.

This is the v1 search driver described in the project's brute-force plan: it owns the
parameterization of a candidate run (currently: steering only -- accelerate held the whole race,
no braking/drifting/tricks/items), the optimizer loop, and exporting a winning run as a real
.rkg ghost file. The engine side (source/host/KBruteSystem.*) just replays whatever per-frame
inputs it's given and reports back the finish time / completion -- it has no notion of "search".

Build the native binary first (from the Kinoko root, with a native, non-Emscripten CMake build
directory -- e.g. `cmake -B build-host && cmake --build build-host --target kinoko_host`), then:

    python tools/brute_force.py --binary build-host/kinoko_host \
        --course 8 --character 0 --vehicle 8 --iterations 500 --out best.rkg

The GUI (tools/brute_gui_server.py) wraps this same BruteForceSearch class so a browser can
configure/start/stop a run and download the result without touching the command line.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Callable, Optional

# ---------------------------------------------------------------------------
# Task file: what we hand to `kinoko_host brute --task <file>` for one evaluation.
# Little-endian, host-native format -- see the matching doc comment in KBruteSystem.cc.
# ---------------------------------------------------------------------------

TASK_MAGIC = b"KBRT"
TASK_HEADER = struct.Struct("<4sIiiiII")  # magic, version, course, character, vehicle, frameCount, maxFrames
TASK_FRAME = struct.Struct("<HBxff")      # buttons, trick, pad, stickX, stickY

BUTTON_ACCELERATE = 0x1
BUTTON_BRAKE = 0x2
BUTTON_ITEM = 0x4
BUTTON_HOP = 0x8


# ---------------------------------------------------------------------------
# Start boost ("perfect start" / "rocket start"): source/game/kart/KartState.cc's own formula
# (calcStartBoost/calcHandleStartBoost), replicated here so the genome and its seeding can target
# it directly instead of relying on the search to rediscover a razor-thin frame window by luck.
#
# While accelerate is held during Countdown, a charge builds toward 1.0 (fast at first, slower as
# it approaches); not holding it decays the charge 4%/frame. Whatever charge exists on the exact
# frame the countdown ends gets locked in against these thresholds. Above the top one is burnout
# (no boost at all) -- holding accelerate for the WHOLE countdown, which is what a naive "always
# accelerate" genome does, overshoots this within ~110 frames and guarantees a burnout every time.
# ---------------------------------------------------------------------------

COUNTDOWN_START_FRAME = 172  # RaceManager.cc STAGE_INTRO_DURATION -- charging can't start before this
GO_FRAME = 411               # verified via KBruteSystem's goFrame field; constant across all
                              # courses/characters/vehicles (172 Intro + 240 Countdown, RaceManager.hh)
_START_BOOST_ENTRIES = [(0.85, 0), (0.88, 10), (0.905, 20), (0.925, 30), (0.94, 45), (0.95, 70)]
_START_BOOST_DELTA_ONE = 0.02
_START_BOOST_DELTA_TWO = 0.002
_START_BOOST_FALLOFF = 0.96


def start_boost_charge(hold_start_frame: float) -> float:
    """The charge value at GO_FRAME if accelerate is held continuously from hold_start_frame
    onward (and not before). Frames before COUNTDOWN_START_FRAME are a no-op either way, since the
    engine doesn't charge until Countdown begins."""
    charge = 0.0
    for frame in range(COUNTDOWN_START_FRAME, GO_FRAME):
        if frame >= hold_start_frame:
            charge += _START_BOOST_DELTA_ONE - (_START_BOOST_DELTA_ONE - _START_BOOST_DELTA_TWO) * charge
        else:
            charge *= _START_BOOST_FALLOFF
        charge = max(0.0, min(1.0, charge))
    return charge


def start_boost_tier(charge: float):
    """Returns (boost_frames, burned_out). boost_frames is 0 if the charge was too low to earn
    anything at all."""
    if charge > _START_BOOST_ENTRIES[-1][0]:
        return 0, True
    prev = 0.0
    for threshold, frames in _START_BOOST_ENTRIES:
        if prev < charge <= threshold:
            return frames, False
        prev = threshold
    return 0, False


def find_best_start_boost_frame():
    """Scans every possible hold-start frame and returns (frame, charge, boost_frames) for the one
    giving the longest boost without burning out. Used to seed the search's start-boost gene near
    the real answer instead of leaving it to find a ~4-frame-wide window by chance."""
    best = (COUNTDOWN_START_FRAME, 0.0, 0)
    for frame in range(COUNTDOWN_START_FRAME, GO_FRAME + 1):
        charge = start_boost_charge(frame)
        boost_frames, burned_out = start_boost_tier(charge)
        if not burned_out and boost_frames > best[2]:
            best = (frame, charge, boost_frames)
    return best


def encode_task(course: int, character: int, vehicle: int, frames, max_frames: int) -> bytes:
    """frames: iterable of (buttons, trick, stickX, stickY)."""
    frames = list(frames)
    header = TASK_HEADER.pack(TASK_MAGIC, 1, course, character, vehicle, len(frames), max_frames)
    body = b"".join(TASK_FRAME.pack(buttons, trick, stickX, stickY)
            for buttons, trick, stickX, stickY in frames)
    return header + body


def parse_result(stdout: str) -> dict:
    for line in stdout.splitlines():
        if line.startswith("RESULT "):
            fields = {}
            for tok in line[len("RESULT "):].split():
                key, _, val = tok.partition("=")
                try:
                    fields[key] = int(val)
                except ValueError:
                    fields[key] = float(val)  # e.g. speed=%.6f
            fields["finished"] = bool(fields["finished"])
            fields["wrongWay"] = bool(fields["wrongWay"])
            return fields
    raise RuntimeError(f"kinoko_host produced no RESULT line:\n{stdout}")


# kinoko_host loads its game assets (Race/Common.szs, Race/Course/<name>.szs) relative to its own
# process's working directory (or $KINOKO_FILESYSTEM_ROOT, if set) -- not relative to the binary's
# own location. Point it at this repo's root (parent of tools/) regardless of the caller's own cwd.
_KINOKO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run_kinoko(binary: str, task_bytes: bytes, timeout: float = 30.0) -> dict:
    with tempfile.NamedTemporaryFile(suffix=".kbrt", delete=False) as f:
        f.write(task_bytes)
        task_path = f.name

    try:
        proc = subprocess.run([binary, "brute", "--task", task_path],
                capture_output=True, text=True, timeout=timeout, cwd=_KINOKO_ROOT)
        if proc.returncode != 0:
            raise RuntimeError(
                    f"kinoko_host exited {proc.returncode}:\nstdout:\n{proc.stdout}\n"
                    f"stderr:\n{proc.stderr}")
        return parse_result(proc.stdout)
    finally:
        os.unlink(task_path)


# ---------------------------------------------------------------------------
# Parameterization: a genome is a list of stick-X control points, one every `step` frames,
# linearly interpolated in between, optionally followed by more control-point channels. Genome
# layout is always: steering (n_points) -> accel channel (n_points, if search_accelerate) OR a
# single start-boost gene (1, if search_start_boost and not search_accelerate) -> trick channel
# (n_points, if search_trick). Sizes are computed explicitly from the flags (genome_gene_count)
# rather than inferred from len(genome), since with three independent channels that's ambiguous.
#
# Two ways to control acceleration are supported:
# - search_start_boost: ONE extra gene, a 0-1 fraction of the countdown mapped to the frame
#   accelerate first gets held down (held CONTINUOUSLY from there onward). Without this,
#   "accelerate held the whole time" is the only option, which guarantees a burned-out start every
#   single run -- see start_boost_charge's comment. Models the pure boost-charge problem exactly
#   (what find_best_start_boost_frame's 305-308 answer assumes) and converges fast since it's one
#   gene, but CANNOT represent tapping accelerate on and off.
# - search_accelerate: a full channel, same shape as steering, each point independently on/off per
#   step-frame block (thresholded, not interpolated -- acceleration is a binary decision, so
#   blending it toward the next block like steering would be meaningless). Subsumes
#   search_start_boost's single continuous hold as one possible pattern among many, at the cost of
#   a much bigger search space. Takes priority over search_start_boost when both are set.
#
# search_trick adds a similar on/off channel for Trick::Up (wheelie). Verified necessary for at
# least one real reference ghost: its countdown-phase position couldn't be reproduced by steering +
# accelerate alone, but matched exactly once its handful of real Trick::Up frames were carried over
# -- a wheelie tap during Countdown does something physically real, not just cosmetic.
# ---------------------------------------------------------------------------


def genome_gene_count(n_points: int, search_start_boost: bool = False,
        search_accelerate: bool = False, search_trick: bool = False) -> int:
    count = n_points
    count += n_points if search_accelerate else (1 if search_start_boost else 0)
    count += n_points if search_trick else 0
    return count


def _stick_raw(x: float) -> int:
    return max(0, min(14, round(x * 7 + 7)))


def _quantize_stick(x: float) -> float:
    """Snaps a continuous stick value to the same 15-position grid the RKG format stores, so a
    live search evaluation can never diverge from what encode_rkg() will actually reproduce."""
    return (_stick_raw(x) - 7) / 7.0


def _quantize_stick_ternary(x: float) -> float:
    """Snaps to only full-left/neutral/full-right (-1, 0, 1), instead of the full 15-position grid --
    see SearchConfig.ternary_steering. Motivated by a real reference ghost (LC_41.105s_41b3f3e)
    which, when inspected, turned out to use ONLY these three values for its entire countdown-phase
    steering (59 full-left, 109 neutral, 71 full-right frames out of 239, zero intermediate values)
    -- a structurally different, discrete-tap steering philosophy from this search's usual noisy,
    continuously-varying output. Constraining the search to the same three values tests whether it
    can find a comparably clean, human-plausible pattern instead of exploiting fine-grained
    per-frame wiggle."""
    if x >= 0.5:
        return 1.0
    if x <= -0.5:
        return -1.0
    return 0.0


def _channels_from_genome(genome, n_points: int, search_start_boost: bool,
        search_accelerate: bool, search_trick: bool):
    """Splits a flat genome into (steering, accel_channel_or_None, accel_start_frame, trick_channel_or_None)
    per the layout documented above."""
    steering, rest = genome[:n_points], genome[n_points:]

    if search_accelerate:
        accel_channel, rest = rest[:n_points], rest[n_points:]
        accel_start_frame = COUNTDOWN_START_FRAME  # unused in this mode
    elif search_start_boost:
        (boost_gene,), rest = rest[:1], rest[1:]
        accel_channel = None
        frac = max(0.0, min(1.0, boost_gene))
        accel_start_frame = COUNTDOWN_START_FRAME + frac * (GO_FRAME - COUNTDOWN_START_FRAME)
    else:
        accel_channel = None
        accel_start_frame = COUNTDOWN_START_FRAME

    trick_channel = rest[:n_points] if search_trick else None
    return steering, accel_channel, accel_start_frame, trick_channel


def frames_from_genome(genome, step: int, total_frames: int, search_start_boost: bool = False,
        search_accelerate: bool = False, search_trick: bool = False, n_points: Optional[int] = None,
        prefix_frames: Optional[list] = None, ternary_steering: bool = False):
    """prefix_frames (segment-chaining, see prefix_frames_from_rkg): when given, frames
    0..len(prefix_frames)-1 are replayed VERBATIM from it -- never touched by the genome -- and the
    genome's own control points instead cover the window starting right where the prefix ends,
    through total_frames. This lets a later segment's search build on an already-locked-in earlier
    segment (e.g. a previous lap) without the engine needing any notion of resuming from a mid-race
    state (it has none -- see KBruteSystem.cc): the prefix is simply re-simulated every evaluation,
    same as the real race would replay it, and only the NEW tail is actually searched. None
    (default) = old single-segment behavior, genome covers from COUNTDOWN_START_FRAME."""
    if n_points is None:
        n_points = len(genome)  # backward-compat: steering-only genome, no other channels
    steering, accel_channel, accel_start_frame, trick_channel = _channels_from_genome(
            genome, n_points, search_start_boost, search_accelerate, search_trick)

    # segment_start is where genome index 0 maps to. Non-prefix (old, default) behavior: genome
    # index 0 = absolute frame 0 (indices before COUNTDOWN_START_FRAME are simply never read, since
    # that whole region always yields neutral padding below -- NOT subtracted out here, since
    # genome_from_frames' own sampling (idx = i * step) assumes this same absolute-index
    # correspondence; offsetting one side without the other would desync every existing seed/export
    # round-trip). Prefix (segment-chaining) case: genome index 0 = right after the locked prefix,
    # a genuinely new window with no prior convention to preserve.
    segment_start = len(prefix_frames) if prefix_frames is not None else 0

    for i in range(total_frames):
        if prefix_frames is not None and i < len(prefix_frames):
            yield prefix_frames[i]
            continue
        if prefix_frames is None and i < COUNTDOWN_START_FRAME:
            # A real ghost's input stream isn't read until Countdown begins (see decode_rkg's
            # docstring), and encode_rkg drops this region entirely on export, replaced by neutral
            # padding when decoded back. If evaluation here used the genome's real interpolated
            # values for this region instead of matching that neutral padding, the search could
            # mutate genes that have a genuine (if small) effect on the *live* evaluation but never
            # survive into the exported file at all -- a real bug this session hit: an accepted
            # "best" scored higher live than its own exported .rkg ever replayed to, because Intro-
            # phase steering/trick noise the search was (wrongly) allowed to exploit vanished on
            # export. Emitting neutral input here instead keeps live scoring and export identical.
            yield (0, 0, 0.0, 0.0)
            continue

        pos = (i - segment_start) / step
        idx = min(int(pos), n_points - 1)
        nxt = min(idx + 1, n_points - 1)
        t = pos - idx if idx != nxt else 0.0
        stick_x = steering[idx] * (1 - t) + steering[nxt] * t
        stick_x = max(-1.0, min(1.0, stick_x))
        stick_x = _quantize_stick_ternary(stick_x) if ternary_steering else _quantize_stick(stick_x)

        if accel_channel is not None:
            buttons = BUTTON_ACCELERATE if accel_channel[idx] > 0 else 0
        else:
            buttons = BUTTON_ACCELERATE if i >= accel_start_frame else 0

        trick = System_Trick_Up if trick_channel is not None and trick_channel[idx] > 0 else 0

        yield (buttons, trick, stick_x, 0.0)


# Matches System::Trick::Up (source/game/system/KPadController.hh) -- the only trick value this
# channel searches, since it's what the reference ghost that motivated this feature actually used.
System_Trick_Up = 1


def random_genome(n_points: int, search_start_boost: bool = False, search_accelerate: bool = False,
        search_trick: bool = False) -> list:
    genome = [random.uniform(-0.3, 0.3) for _ in range(n_points)]
    if search_accelerate:
        # Mostly-on: accelerating is usually right, so bias the starting population there and let
        # mutation discover where OFF windows actually help, rather than starting from noise that's
        # ~50% off and almost certainly can't even leave the start line.
        genome += [random.uniform(0.2, 1.0) for _ in range(n_points)]
    elif search_start_boost:
        # Seed near the analytically-derived best frame (see find_best_start_boost_frame) rather
        # than uniform over [0, 1] -- the safe window is only ~4 frames wide out of ~240, so a
        # blind random start would need a huge number of iterations to land in it by chance.
        best_frame, _charge, _boost = find_best_start_boost_frame()
        best_frac = (best_frame - COUNTDOWN_START_FRAME) / (GO_FRAME - COUNTDOWN_START_FRAME)
        genome.append(max(0.0, min(1.0, random.gauss(best_frac, 0.01))))
    if search_trick:
        # Mostly-off: unlike accelerating, tricking is the exception, not the rule -- bias there so
        # a random start doesn't spend most of the countdown wheelieing for no reason.
        genome += [random.uniform(-1.0, -0.2) for _ in range(n_points)]
    return genome


def mutate(genome: list, rate: float, sigma: float, special_index: Optional[int] = None,
        special_sigma: Optional[float] = None) -> list:
    """special_sigma overrides sigma for genome[special_index] only -- used for the start-boost
    gene, whose safe window (~4 frames out of ~240) is far too narrow for the general steering
    mutation scale. special_index is an explicit position (NOT always -1: with search_trick also
    on, the boost gene sits before the trick channel, not at the end of the genome)."""
    out = [g + random.gauss(0, sigma) if random.random() < rate else g for g in genome]
    if special_index is not None and special_sigma is not None:
        g = genome[special_index]
        out[special_index] = g + random.gauss(0, special_sigma) if random.random() < rate else g
    return out


def _cpu_run_many(binary: str, course: int, character: int, vehicle: int):
    """The default `run_many` for the reference builders below: each job is (frames, max_frames),
    evaluated by its own kinoko_host subprocess. tools/gpu_brute.py passes a GPU-batched one
    instead, which returns the same result dicts."""
    return lambda jobs: [run_kinoko(binary, encode_task(course, character, vehicle, f, m))
            for f, m in jobs]


def compute_progress_reference(binary: str, reference_rkg: str, course: int, character: int,
        vehicle: int, start_frame: int, end_frame: int, num_segments: int = 20, run_many=None):
    """Derives the "progress" fitness target from a reference .rkg: a piecewise-linear polyline
    sampling its own path from start_frame to end_frame at num_segments waypoints, plus its own
    raceCompletion at end_frame. Returns a dict: {"segments": [...], "total_length": float,
    "reference_completion": int}. Candidates are scored by projecting their own position onto
    whichever segment gives the highest "distance already covered along this polyline" (see
    piecewise_progress) -- rewarding progress along the reference's own racing line AS IT ACTUALLY
    CURVES, not a single straight line between just the two endpoints.

    A single start-to-end straight line (the original design) works fine when the reference's path
    barely curves over the scored window, but breaks down on a real curve: a candidate can rack up
    a higher straight-line projection by cutting straighter and covering more raw distance along
    that fixed line while actually turning LESS into the curve than the reference did -- a worse
    real racing line despite scoring higher. Confirmed in practice on a course with a sharper turn
    within the scored window: a candidate that beat the reference on straight-line progress visibly
    lagged behind it in Kinoko's own web viewer, and lagged specifically on the axis matching how
    far around the turn the kart had actually come. More waypoints make the polyline hug the real
    path more closely; num_segments=20 is a reasonable default (each extra waypoint costs one more
    one-time reference simulation, not a per-candidate cost, so this is cheap to raise if needed).

    reference_completion (the engine's real, checkpoint-based, track-curve-aware race position) is
    still used in fitness() as a hard "must be at least this far along the real track" gate on top
    of the continuous polyline score, since even a fine polyline is still an approximation.

    start_frame is expected to be at or before COUNTDOWN_START_FRAME-ish (the kart doesn't move
    under its own power until Countdown ends regardless of input, so any frame up to about
    GO_FRAME works equally well as the "start" point -- see decode_rkg's docstring)."""
    _course, _character, _vehicle, frames = decode_rkg(reference_rkg)
    run_many = run_many or _cpu_run_many(binary, course, character, vehicle)

    waypoint_frames = [start_frame + round(i * (end_frame - start_frame) / num_segments)
            for i in range(num_segments + 1)]
    jobs = [(frames[:f], f) for f in waypoint_frames]
    if end_frame not in waypoint_frames:
        # end_frame wasn't exactly one of the rounded waypoints (rounding can land ties/off-by-one
        # for num_segments that don't evenly divide the span) -- fetch it directly.
        jobs.append((frames[:end_frame], end_frame))
    results = run_many(jobs)
    positions = [(r["posX"], r["posY"], r["posZ"]) for r in results[:len(waypoint_frames)]]
    reference_completion = results[-1]["completion"] if end_frame not in waypoint_frames else \
            results[waypoint_frames.index(end_frame)]["completion"]

    segments = []
    cumulative = 0.0
    for a, b in zip(positions, positions[1:]):
        vec = tuple(bb - aa for aa, bb in zip(a, b))
        seg_length = sum(d * d for d in vec) ** 0.5
        unit = tuple(d / seg_length for d in vec) if seg_length > 1e-6 else (0.0, 0.0, 0.0)
        segments.append({"start": a, "unit": unit, "length": seg_length, "before": cumulative})
        cumulative += seg_length

    if cumulative < 1.0:
        raise ValueError(
                f"reference ghost barely moved between frame {start_frame} and {end_frame} "
                f"({cumulative:.2f} units) -- can't derive a meaningful progress path from it")

    # For an optional extra Z-axis bonus on top of the piecewise score (see fitness()'s
    # completion_bonus_weight-style docstring for z_bonus_weight) -- z_sign is +1 or -1 so "more Z
    # progress" always means "the same direction the reference itself moved in Z", regardless of
    # whether this track section happens to increase or decrease Z as the kart advances.
    z_start = positions[0][2]
    z_end = positions[-1][2]
    z_sign = 1.0 if z_end >= z_start else -1.0

    return {"segments": segments, "total_length": cumulative,
            "reference_completion": reference_completion, "z_start": z_start, "z_sign": z_sign}


def _signed_heading_delta(base_front, cur_front) -> float:
    """Signed angle (degrees) from base_front to cur_front, both (frontX, frontZ) ground-plane
    facing vectors from the engine's own bodyFront() (see KBruteSystem.cc) -- computed via 2D
    cross/dot instead of subtracting atan2() angles, since the kart's actual heading values sit
    right at the +-180 wraparound boundary (it starts facing frontZ=-1) where naive subtraction
    would misfire. Positive = cur_front is rotated counterclockwise from base_front (in this
    engine's XZ convention), negative = clockwise."""
    bx, bz = base_front
    cx, cz = cur_front
    cross = bx * cz - bz * cx
    dot = bx * cx + bz * cz
    return math.degrees(math.atan2(cross, dot))


def compute_heading_reference(binary: str, reference_rkg: str, course: int, character: int,
        vehicle: int, checkpoint_frame: int, run_many=None):
    """Derives a fitness target for 'how far has the kart turned by a specific checkpoint frame',
    motivated by a real observation: reference ghosts can commit to a turn direction well before
    the end of a scored window (e.g. LC_41.105s_41b3f3e is already ~10 degrees rotated from its
    start orientation by frame 234, well before frame 412), and it's not obvious the search's usual
    end-of-window scoring alone puts enough pressure on getting there early. Returns
    {"checkpoint_frame": int, "sign": +-1.0, "magnitude": float (degrees)} -- sign/magnitude
    together describe the reference's own signed rotation from its fixed start orientation
    (frame COUNTDOWN_START_FRAME, identical for every candidate since input has no effect before
    Countdown begins) to checkpoint_frame. See fitness()'s heading_bonus_weight docstring for how
    this is used to score candidates."""
    _course, _character, _vehicle, frames = decode_rkg(reference_rkg)
    run_many = run_many or _cpu_run_many(binary, course, character, vehicle)
    base_result, cp_result = run_many([(frames[:COUNTDOWN_START_FRAME], COUNTDOWN_START_FRAME),
            (frames[:checkpoint_frame], checkpoint_frame)])
    base_front = (base_result["frontX"], base_result["frontZ"])
    cp_front = (cp_result["frontX"], cp_result["frontZ"])

    delta = _signed_heading_delta(base_front, cp_front)
    return {"checkpoint_frame": checkpoint_frame, "base_front": base_front,
            "sign": 1.0 if delta >= 0 else -1.0, "magnitude": abs(delta)}


def piecewise_progress(pos, progress_ref) -> float:
    """How far `pos` projects along the reference's polyline (see compute_progress_reference),
    taking the max over every segment of (that segment's own cumulative distance-before + `pos`'s
    projection onto that segment's direction). Taking the max rather than "whichever segment is
    nearest" makes this robust to a candidate that's cut a corner or drifted off the reference's
    exact line -- it always credits the interpretation that gives the furthest honest progress,
    the same way a real racing line's checkpoint system doesn't care how far sideways you are, only
    how far along the track you've come."""
    best = float("-inf")
    for seg in progress_ref["segments"]:
        disp = tuple(p - s for p, s in zip(pos, seg["start"]))
        projected = sum(d * u for d, u in zip(disp, seg["unit"]))
        total = seg["before"] + projected
        if total > best:
            best = total
    return best


def fitness(result: dict, metric: str = "completion", progress_ref=None,
        boost_bonus_weight: float = 0.0, completion_bonus_weight: float = 0.0,
        z_bonus_weight: float = 0.0, completion_tolerance: int = 1,
        min_boost_charge: Optional[float] = None, speed_bonus_weight: float = 0.0,
        max_boost_charge: Optional[float] = None, accel_hold_bonus_weight: float = 0.0,
        heading_bonus_weight: float = 0.0) -> float:
    """Lower is better. Finished runs are always ranked by time (a genuine finish beats any partial
    attempt regardless of metric). Unfinished runs are ranked by `metric`:

    - "completion": raceCompletion, checkpoint-based. Right for a full lap/race search, but it's
      quantized to checkpoints -- over a short window (a start slide, a few hundred frames) very
      different attempts can read identically simply because none of them reached the next
      checkpoint yet, which gives the search nothing to climb. Verified empirically: four wildly
      different steering patterns over the same 450-frame window all produced the exact same
      completion value.
    - "speed": the kart's raw forward speed at the cutoff frame. Changes every frame regardless of
      checkpoints, and is the metric TAS players actually judge a start slide / rocket start by --
      use this whenever max_frames is short enough that completion alone stays flat.
    - "progress": distance projected along a piecewise-linear polyline of the reference ghost's own
      path (see compute_progress_reference / piecewise_progress) -- rewards going further along the
      SAME curving line the reference took, not just faster or further in any fixed direction.
      progress_ref is the dict from compute_progress_reference; required when metric == "progress".
      A candidate whose own raceCompletion doesn't beat reference_completion is penalized hard (see
      compute_progress_reference's docstring for why even a fine polyline is still an
      approximation, and this hard gate against the engine's own real position metric remains).

    boost_bonus_weight adds an explicit reward for start-boost quality on top of whichever metric
    above is used, using the engine's own reported startBoostCharge (result["boostCharge"] --
    ground truth from KartState::calcStartBoost, the exact real formula, not a Python
    reimplementation) run through start_boost_tier() to get the actual boost duration a candidate
    earns. This exists because NEITHER "progress" nor "speed" naturally rewards a good charge on
    their own: a short max_frames window that ends at/near GO_FRAME can't see the boosted speed
    that would follow, so without this term the search has zero pressure toward finding a genuine
    full boost and can converge on an accelerate pattern that's great for pre-GO positioning while
    being accidentally garbage for charge (confirmed in practice: search results that scored well
    on "progress" alone turned out not to trigger any boost at all when replayed in real Dolphin).
    0.0 (default) preserves old behavior exactly.

    completion_bonus_weight adds a CONTINUOUS reward for how far a candidate's own raceCompletion
    exceeds reference_completion (metric == "progress" only), on top of the hard gate above. The
    gate alone only guarantees "not behind at the one frame it checks" -- it creates no pressure to
    actually pull ahead, and only checks the final scored frame, so a candidate can satisfy it while
    being merely TIED (or briefly behind) at earlier frames within the same window. Confirmed in
    practice: a search using the gate alone converged on a result that tied the reference's
    completion and matched a straight-line-friendly axis while actually trailing on the track's own
    curve at an earlier frame -- the gate had nothing to say about that, since nothing rewarded
    exceeding completion, only avoiding a same-frame deficit at the end. 0.0 (default) preserves old
    behavior exactly (gate only, no continuous pull).

    z_bonus_weight adds an explicit EXTRA reward for progress specifically along the Z axis, in
    whichever direction the reference itself moved in Z (progress_ref["z_sign"]) -- on top of, not
    instead of, the piecewise polyline score above. The polyline already blends X and Z following
    the reference's real path, so this doesn't override that; it just lets you dial up how much
    extra a candidate gets credited for matching the reference's Z progress specifically (e.g. how
    far around a turn it's come) without discarding the credit the polyline still gives for X/
    rightward distance. 0.0 (default) preserves old behavior exactly (no extra Z-specific pull).

    completion_tolerance relaxes the hard gate to completion >= reference_completion -
    completion_tolerance instead of an exact >=. raceCompletion is coarsely quantized (see the
    "completion" metric note above), so when seed and reference are the same near-optimal file,
    the majority of small mutations produce a trajectory that's essentially as good as the
    reference but happens to round its completion down by one unit purely from quantization noise
    -- with zero tolerance those get the FULL 1e9 rejection penalty despite not being meaningfully
    behind, which measurably starves the search of viable candidates. Confirmed empirically: with
    tolerance=0, 105 of 200 random mutations from an already-good seed were rejected by the gate,
    and only 1 of the remaining ~95 improved progress -- the gate was discarding roughly half of
    the search's entire budget on quantization noise, not real regressions. 1 (default) allows a
    one-unit rounding margin; 0 restores the old exact-match behavior.

    min_boost_charge is a hard gate on start-boost quality: a candidate whose result["boostCharge"]
    falls below this threshold is rejected with the same full 1e9 penalty as the completion gate,
    regardless of metric. This exists because start_boost_tier's charge->duration mapping has a cliff
    at 0.94 (0.925-0.94 gives only 45 boost frames, 0.94-0.95 gives 70) -- boost_bonus_weight alone
    only makes the 70-frame tier PREFERRED, it doesn't forbid the search from drifting back below
    0.94 for a candidate that gains a little elsewhere (e.g. progress) while losing the tier, which
    is a bad trade every time (dropping from 70 to 45 boost frames costs far more than any single
    candidate's other gains). None (default) = no gate (old behavior).

    speed_bonus_weight adds an explicit reward for the kart's raw forward speed AT THE FINAL SCORED
    FRAME (result["speed"]), on top of whichever metric is used -- for "progress"/"completion" this
    is the only thing that rewards the kart actually being in a good, forward-moving state when the
    window ends, as opposed to merely having covered good cumulative distance/position. Without it,
    a candidate that reaches a great position through a lot of erratic/oscillating steering (never
    fully committing to one turn direction) can score just as well as one that ends pointed cleanly
    forward and accelerating, even though the erratic one is effectively stalled or mid-spin at the
    cutoff -- confirmed in practice: a search result with completion nearly tied with a real
    reference ghost had speed ~0 at the cutoff frame vs the reference's speed of 3.24, despite
    identical boost charge/tier, traced to the reference holding a clean full-lock turn (stickX=1.0)
    for its last several frames while the search's result wavered in the 0.7-0.86 range and never
    committed. This matters even more for segment-chained search (SearchConfig.prefix_rkg), since a
    segment that ends stalled/mid-spin hands the NEXT segment a bad starting state it likely can't
    recover from. 0.0 (default) preserves old behavior exactly (no speed pressure at the cutoff).

    max_boost_charge is the mirror of min_boost_charge: a hard gate rejecting any candidate whose
    boostCharge EXCEEDS this value, with the same full 1e9 penalty. Above 0.95 is a full burnout
    (start_boost_tier returns 0 boost frames -- see _START_BOOST_ENTRIES), so without this a
    candidate could drift past the ceiling and lose the boost entirely; boost_bonus_weight alone
    only makes staying under it preferred (via the lost 70*weight bonus), it doesn't forbid crossing
    it outright. None (default) = no gate (old behavior, matches min_boost_charge's default).

    accel_hold_bonus_weight rewards result["accelFramesHeld"] -- the total number of Countdown-phase
    frames (COUNTDOWN_START_FRAME to GO_FRAME) the candidate held accelerate, computed in
    BruteForceSearch._evaluate() from the actual per-frame input sequence (fitness() itself never
    sees per-frame data, only the engine's final-state result). This is a DIFFERENT axis from
    boost_bonus_weight: boost_bonus_weight only cares about the charge value's final tier (a
    razor-thin ~4-frame window gives the max), while this rewards accelerate hold TIME itself,
    independent of charge -- motivated by community TAS knowledge (see this project's Discord
    research) that holding accelerate longer during the countdown affects kart rotation/slide
    distance during airtime, a real effect separate from the charge-to-boost-duration formula. Use
    alongside max_boost_charge=0.95 so the search can freely push hold-time up without accidentally
    burning out the charge in the process. 0.0 (default) = no hold-time pressure (old behavior).

    heading_bonus_weight rewards result["headingProgress"] -- how far the kart has turned, by
    SearchConfig.heading_checkpoint_frame, in the same direction the reference ghost itself turned
    by that frame (see compute_heading_reference), computed in BruteForceSearch._evaluate() via an
    extra engine evaluation cut at that checkpoint. Motivated by a real observation: a reference
    ghost can already be meaningfully rotated toward a turn well before the end of the scored window
    (e.g. ~10 degrees by frame 234 out of a 412-frame window) -- without this, nothing rewards
    committing to that rotation EARLY specifically, only wherever the final-frame metrics happen to
    reward it. 0.0 (default) = no early-rotation pressure (old behavior).
    """
    if result["finished"]:
        return float(result["timeMs"])

    boost_gate_penalty = 0.0
    if (min_boost_charge is not None and "boostCharge" in result
            and result["boostCharge"] < min_boost_charge):
        boost_gate_penalty = 1_000_000_000.0
    if (max_boost_charge is not None and "boostCharge" in result
            and result["boostCharge"] > max_boost_charge):
        boost_gate_penalty = 1_000_000_000.0

    boost_bonus = 0.0
    if boost_bonus_weight and "boostCharge" in result:
        boost_frames, _burned_out = start_boost_tier(result["boostCharge"])
        boost_bonus = boost_bonus_weight * boost_frames

    speed_bonus = speed_bonus_weight * result.get("speed", 0.0)
    accel_hold_bonus = accel_hold_bonus_weight * result.get("accelFramesHeld", 0)
    heading_bonus = heading_bonus_weight * result.get("headingProgress", 0.0)

    if metric == "speed":
        return -result["speed"] - boost_bonus - accel_hold_bonus - heading_bonus + boost_gate_penalty
    if metric == "progress":
        reference_completion = progress_ref["reference_completion"]
        pos = (result["posX"], result["posY"], result["posZ"])
        projected = piecewise_progress(pos, progress_ref)
        # Hard gate: a candidate behind the reference on the game's OWN (track-curve-aware)
        # position metric is never preferred over one that's ahead, no matter how much higher its
        # polyline projection scores -- see compute_progress_reference's docstring. The penalty
        # (1e9) dwarfs any realistic projected/boost value, so this acts like a strict lexicographic
        # priority (ahead-by-completion, THEN projected distance) without needing actual tuple/
        # lexicographic comparison through the rest of the search loop.
        behind_penalty = (0.0 if result["completion"] >= reference_completion - completion_tolerance
                else 1_000_000_000.0)
        completion_bonus = completion_bonus_weight * (result["completion"] - reference_completion)
        z_progress = (result["posZ"] - progress_ref["z_start"]) * progress_ref["z_sign"]
        z_bonus = z_bonus_weight * z_progress
        return (-projected - boost_bonus - completion_bonus - z_bonus - speed_bonus - accel_hold_bonus
                - heading_bonus + behind_penalty + boost_gate_penalty)
    return (1_000_000_000.0 - result["completion"] - boost_bonus - speed_bonus - accel_hold_bonus
            - heading_bonus + boost_gate_penalty)


# ---------------------------------------------------------------------------
# Search loop
# ---------------------------------------------------------------------------


@dataclass
class SearchConfig:
    binary: str
    course: int
    character: int
    vehicle: int
    iterations: int = 500
    n_points: int = 24
    step: int = 15          # frames between control points
    max_frames: int = 10800  # 3 minutes @ 60fps safety cap
    mutation_rate: float = 0.3
    mutation_sigma: float = 0.15
    seed_genome: Optional[list] = None  # starting point instead of random_genome, e.g. from a
                                         # previous run's .rkg via genome_from_rkg -- used to refine
                                         # a known-good attempt (a start slide, a corner cut, ...)
                                         # instead of searching from scratch every time.
    fitness_metric: str = "completion"  # "completion" for a full lap/race, "speed" for a short
                                         # window (start slides) -- see fitness()'s docstring.
    search_start_boost: bool = False    # add the single accel-hold-start-frame gene; turn this on
                                         # for a pure start-boost search, otherwise every attempt
                                         # holds accelerate the whole countdown and always burns out.
                                         # Ignored when search_accelerate is also set.
    search_accelerate: bool = False     # add a full accelerate on/off channel (one gene per
                                         # steering control point, same step) instead of the single
                                         # start-boost gene -- needed to represent a real multi-tap
                                         # accelerate pattern (e.g. one that also triggers drift/hop
                                         # timing into a corner), at the cost of double the genes to
                                         # search. Takes priority over search_start_boost.
    search_trick: bool = False          # add a Trick::Up (wheelie) on/off channel, same shape as
                                         # the accelerate channel. Verified necessary for at least
                                         # one real reference ghost -- see frames_from_genome's
                                         # module comment.
    start_boost_mutation_sigma: float = 0.005  # ~1.2 frames -- the safe window is only ~4 frames
                                                # wide out of ~240, so this gene needs a much finer
                                                # mutation scale than steering's.
    reference_rkg: Optional[str] = None        # required when fitness_metric == "progress": the
                                                # ghost whose own direction of travel candidates are
                                                # scored against (see compute_progress_reference).
    progress_start_frame: int = COUNTDOWN_START_FRAME  # where the reference displacement starts
                                                        # from; the end is this run's own max_frames.
    parallel_workers: int = 1  # evaluate this many independently-mutated candidates per generation
                                # concurrently (each is its own kinoko_host subprocess -- CPU-bound
                                # work in a child process, so a thread pool parallelizes real work
                                # despite the GIL) and keep the best of the batch. 1 = old behavior,
                                # one mutation per iteration. "iterations" counts generations, not
                                # individual evaluations, so total evaluations scale with this too.
    boost_bonus_weight: float = 0.0  # explicit reward for start-boost quality -- see fitness()'s
                                      # docstring. 0.0 = old behavior (no boost pressure at all).
                                      # A candidate earning the top tier (70 boost frames) gets
                                      # +70*weight added to its score, so pick a weight relative to
                                      # your fitness_metric's typical scale -- e.g. weight=3.0 gives
                                      # up to +210, a strong pull for a "progress" score in the
                                      # hundreds without completely overriding positional quality.
    min_boost_charge: Optional[float] = None  # hard gate: reject any candidate whose boostCharge
                                               # falls below this -- see fitness()'s docstring. Use
                                               # 0.94 to forbid ever dropping out of the 70-boost-
                                               # frame tier, since boost_bonus_weight alone only
                                               # prefers staying in it, it doesn't forbid leaving.
                                               # None (default) = no gate (old behavior).
    speed_bonus_weight: float = 0.0  # explicit reward for raw forward speed at the final scored
                                      # frame -- see fitness()'s docstring. Without this, a candidate
                                      # can score well on cumulative position/completion while ending
                                      # mid-spin/stalled (erratic steering that never commits to one
                                      # turn direction), which especially hurts segment-chained search
                                      # since the next segment inherits that bad state. 0.0 (default)
                                      # = old behavior (no speed pressure at the cutoff).
    max_boost_charge: Optional[float] = None  # hard gate, mirror of min_boost_charge: reject any
                                               # candidate whose boostCharge EXCEEDS this -- see
                                               # fitness()'s docstring. Use 0.95 to forbid ever
                                               # crossing into full burnout (0 boost frames). None
                                               # (default) = no gate (old behavior).
    accel_hold_bonus_weight: float = 0.0  # reward for total Countdown-phase frames spent holding
                                           # accelerate, independent of the resulting charge tier --
                                           # see fitness()'s docstring. Pair with max_boost_charge=0.95
                                           # so the search can push hold-time up without accidentally
                                           # burning out the charge. 0.0 (default) = old behavior.
    ternary_steering: bool = False  # constrain steering to only full-left/neutral/full-right (-1,
                                     # 0, 1) instead of the full 15-position grid -- see
                                     # _quantize_stick_ternary's docstring. False (default) = old
                                     # behavior (full 15-position grid).
    heading_checkpoint_frame: Optional[int] = None  # frame at which to score how far the kart has
                                                     # turned toward the reference's own turn
                                                     # direction -- see compute_heading_reference and
                                                     # fitness()'s heading_bonus_weight docstring.
                                                     # Requires reference_rkg. None (default) = no
                                                     # checkpoint scoring (old behavior, and no extra
                                                     # per-candidate engine evaluation cost).
    heading_bonus_weight: float = 0.0  # reward for turning toward the reference's own direction by
                                        # heading_checkpoint_frame -- see fitness()'s docstring.
                                        # 0.0 (default) = old behavior (no early-rotation pressure).
    completion_bonus_weight: float = 0.0  # continuous reward for exceeding the reference's own
                                           # raceCompletion -- see fitness()'s docstring. 0.0 = old
                                           # behavior (the hard gate alone, no pull toward pulling
                                           # further ahead on the track's own curve, only a floor
                                           # against falling behind at the final scored frame).
    z_bonus_weight: float = 0.0  # explicit EXTRA reward for Z-axis progress specifically, on top
                                  # of (not instead of) the piecewise polyline score -- see
                                  # fitness()'s docstring. 0.0 = old behavior (no extra Z pull).
    completion_tolerance: int = 1  # rounding margin on the completion gate -- see fitness()'s
                                    # docstring. Confirmed necessary: with 0, over half of random
                                    # mutations from an already-good seed got rejected purely from
                                    # raceCompletion's quantization noise, not real regressions.
    stagnation_stop_after: Optional[int] = None  # stop the search early once this many generations
                                                  # have passed with no accepted improvement -- a
                                                  # heuristic "probably converged" signal, not a
                                                  # proof (this is a stochastic hill-climb, not an
                                                  # exhaustive search, so further improvement is
                                                  # never strictly ruled out). None = never auto-stop
                                                  # (old behavior); always runs the full iteration
                                                  # count regardless of stagnation.
    eval_log_path: Optional[str] = None  # append a JSONL record (genome, progress, completion,
                                          # boostCharge, config metadata) for every evaluated
                                          # candidate, across runs -- opt-in, since it's extra disk
                                          # I/O every evaluation. Feeds tools/train_surrogate.py.
                                          # None (default) = no logging (old behavior).
    surrogate_path: Optional[str] = None  # a trained surrogate model (see tools/surrogate.py /
                                           # train_surrogate.py) used to cheaply pre-rank a larger
                                           # pool of candidates per generation, spending real (slow)
                                           # engine evaluation only on the most promising subset --
                                           # see _run()'s surrogate-filtering comment. None
                                           # (default) = no filtering (old behavior, every generated
                                           # candidate gets a real evaluation). Only used when
                                           # fitness_metric == "progress" and parallel_workers > 1.
    surrogate_oversample: int = 4  # generate this many candidates per real evaluation slot when a
                                    # surrogate is active (e.g. parallel_workers=8, oversample=4 ->
                                    # 32 candidates proposed, surrogate-ranked, top 8 real-evaluated).
    prefix_rkg: Optional[str] = None  # segment-chaining: a previous segment's WINNING .rkg (e.g.
                                       # lap 1's finished search), replayed verbatim as a locked
                                       # prefix every evaluation -- see frames_from_genome's
                                       # prefix_frames param and prefix_frames_from_rkg. This run's
                                       # genome then only searches the NEW frames from where that
                                       # prefix ends through max_frames, letting a long race be
                                       # optimized piece by piece without the engine needing any
                                       # notion of resuming from a mid-race state (it has none). Set
                                       # progress_start_frame to prefix_frame_count (or the prefix's
                                       # full length) so fitness_metric="progress" scores only the
                                       # NEW segment, not the whole race from frame 0. None (default)
                                       # = old single-segment behavior (genome covers the whole race
                                       # from COUNTDOWN_START_FRAME).
    prefix_frame_count: Optional[int] = None  # how many frames of prefix_rkg to lock; None (default)
                                               # takes its entire recorded sequence.


class BruteForceSearch:
    """Runs a (mu+1) hill-climb in a background thread. Call start(), poll status(), stop()."""

    def __init__(self, config: SearchConfig, on_status: Optional[Callable[[dict], None]] = None):
        self.config = config
        self._on_status = on_status
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._status = {
            "running": False,
            "iteration": 0,
            "totalIterations": config.iterations,
            "bestFitness": None,
            "bestFinished": False,
            "bestTimeMs": None,
            "bestCompletion": 0,
            "bestSpeed": None,
            "bestProgress": None,
            "referenceProgress": None,
            "progressDelta": None,
            "referenceCompletion": None,
            "completionDelta": None,
            "bestBoostCharge": None,
            "bestBoostFrames": None,
            "iterationsSinceImprovement": 0,
            "stagnant": False,
            "msPerIteration": None,
            "etaSeconds": None,
            "error": None,
        }
        self._best_genome: Optional[list] = None
        self._best_result: Optional[dict] = None
        self._progress_ref = None
        self._start_time: Optional[float] = None
        self._locked_prefix: Optional[list] = None
        self._heading_ref = None

    def status(self) -> dict:
        with self._lock:
            return dict(self._status)

    def best_snapshot(self):
        """Like best(), but also returns the progress score for that EXACT genome/result pair,
        computed fresh under the same lock acquisition. status()["bestProgress"] is a separate
        snapshot that a background iteration can update in between two separate lock acquisitions
        (best() then status()) -- callers that need a self-consistent (genome, result, progress)
        triple, like exporting a .rkg with a labeled filename, must use this instead of composing
        best() + status()."""
        with self._lock:
            genome, result = self._best_genome, self._best_result
            progress = self._progress_of(result, self._progress_ref) if result is not None else None
            return genome, result, progress

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

    def best(self):
        with self._lock:
            return self._best_genome, self._best_result

    def _set_status(self, **kwargs):
        with self._lock:
            self._status.update(kwargs)
            snapshot = dict(self._status)
        if self._on_status:
            self._on_status(snapshot)

    def locked_prefix_frames(self):
        """The locked prefix this run's genome is searching on top of (see SearchConfig.prefix_rkg),
        or None for a normal single-segment run. Set once at the start of _run(); exposed so callers
        exporting a .rkg (e.g. brute_gui_server.py's _download_rkg) can pass the same prefix to
        frames_from_genome that live evaluation used, without re-reading prefix_rkg themselves."""
        return self._locked_prefix

    def _evaluate(self, genome) -> dict:
        cfg = self.config
        frames = list(frames_from_genome(genome, cfg.step, cfg.max_frames, cfg.search_start_boost,
                cfg.search_accelerate, cfg.search_trick, cfg.n_points, self._locked_prefix,
                cfg.ternary_steering))
        task = encode_task(cfg.course, cfg.character, cfg.vehicle, frames, cfg.max_frames)
        result = run_kinoko(cfg.binary, task)
        # Total Countdown-phase frames with accelerate held -- see fitness()'s accel_hold_bonus_weight
        # docstring. Computed here (not in fitness()) since it needs the actual per-frame input
        # sequence, which fitness() never sees (only the engine's final-state result dict).
        result["accelFramesHeld"] = sum(1 for i in range(COUNTDOWN_START_FRAME, min(GO_FRAME, cfg.max_frames))
                if frames[i][0] & BUTTON_ACCELERATE)
        if self._heading_ref is not None:
            # Extra engine evaluation cut at the checkpoint frame -- see fitness()'s
            # heading_bonus_weight docstring. Reuses the SAME frames this candidate already
            # generated (just truncated), so it's evaluating the exact same input sequence up to
            # that point, not a separately-mutated one.
            cp_frame = self._heading_ref["checkpoint_frame"]
            cp_task = encode_task(cfg.course, cfg.character, cfg.vehicle, frames[:cp_frame], cp_frame)
            cp_result = run_kinoko(cfg.binary, cp_task)
            cur_front = (cp_result["frontX"], cp_result["frontZ"])
            delta = _signed_heading_delta(self._heading_ref["base_front"], cur_front)
            # Oriented so "more positive" always means "more rotated in the SAME direction the
            # reference itself turned by this checkpoint" -- same convention as z_bonus_weight's
            # z_sign, regardless of which way that happens to be for this particular turn.
            result["headingProgress"] = delta * self._heading_ref["sign"]
        if cfg.eval_log_path:
            from surrogate import log_evaluation
            progress = self._progress_of(result, self._progress_ref)
            log_evaluation(cfg.eval_log_path, genome, progress, result["completion"],
                    result.get("boostCharge"), self._surrogate_meta)
        return result

    def _progress_of(self, result: dict, progress_ref) -> Optional[float]:
        if progress_ref is None:
            return None
        pos = (result["posX"], result["posY"], result["posZ"])
        return piecewise_progress(pos, progress_ref)

    def _run(self):
        cfg = self.config
        self._set_status(running=True, error=None)
        try:
            self._locked_prefix = (prefix_frames_from_rkg(cfg.prefix_rkg, cfg.prefix_frame_count)
                    if cfg.prefix_rkg else None)
            self._heading_ref = None
            if cfg.heading_checkpoint_frame is not None:
                if not cfg.reference_rkg:
                    raise ValueError("heading_checkpoint_frame requires reference_rkg")
                self._heading_ref = compute_heading_reference(cfg.binary, cfg.reference_rkg,
                        cfg.course, cfg.character, cfg.vehicle, cfg.heading_checkpoint_frame)
            self._surrogate_meta = None
            surrogate_model = None
            if cfg.eval_log_path or cfg.surrogate_path:
                from surrogate import SurrogateMeta
                self._surrogate_meta = SurrogateMeta(cfg.course, cfg.character, cfg.vehicle,
                        cfg.n_points, cfg.step, cfg.search_start_boost, cfg.search_accelerate,
                        cfg.search_trick)
            if cfg.surrogate_path:
                if cfg.fitness_metric != "progress":
                    raise ValueError("surrogate_path requires fitness_metric='progress' -- the "
                            "surrogate predicts piecewise progress, which is only computed for "
                            "that metric")
                from surrogate import SurrogateModel
                surrogate_model = SurrogateModel.load(cfg.surrogate_path)
                if not surrogate_model.meta.matches(self._surrogate_meta):
                    raise ValueError(
                            f"surrogate model at {cfg.surrogate_path} was trained for a different "
                            f"course/character/vehicle/genome-shape config ({surrogate_model.meta}) "
                            f"than this search ({self._surrogate_meta})")

            progress_ref = None
            if cfg.fitness_metric == "progress":
                if not cfg.reference_rkg:
                    raise ValueError("fitness_metric='progress' requires reference_rkg")
                self._progress_ref = progress_ref = compute_progress_reference(cfg.binary,
                        cfg.reference_rkg, cfg.course, cfg.character, cfg.vehicle,
                        cfg.progress_start_frame,
                        cfg.max_frames)

            # The reference's own scores for THIS window, reported alongside every status update so
            # "are we actually ahead of the reference, not just self-consistent" is visible live
            # instead of requiring a separate manual comparison after the fact.
            reference_progress = progress_ref["total_length"] if progress_ref else None
            reference_completion = progress_ref["reference_completion"] if progress_ref else None

            gene_count = genome_gene_count(cfg.n_points, cfg.search_start_boost,
                    cfg.search_accelerate, cfg.search_trick)
            if cfg.seed_genome is not None:
                genome = list(cfg.seed_genome[:gene_count])
                genome += [0.0] * (gene_count - len(genome))
            else:
                genome = random_genome(cfg.n_points, cfg.search_start_boost, cfg.search_accelerate,
                        cfg.search_trick)
            boost_gene_active = cfg.search_start_boost and not cfg.search_accelerate
            boost_gene_index = cfg.n_points if boost_gene_active else None
            boost_gene_sigma = cfg.start_boost_mutation_sigma if boost_gene_active else None
            self._start_time = time.time()
            result = self._evaluate(genome)
            best_fit = fitness(result, cfg.fitness_metric, progress_ref, cfg.boost_bonus_weight,
                    cfg.completion_bonus_weight, cfg.z_bonus_weight, cfg.completion_tolerance,
                    cfg.min_boost_charge, cfg.speed_bonus_weight, cfg.max_boost_charge,
                    cfg.accel_hold_bonus_weight, cfg.heading_bonus_weight)
            self._best_genome, self._best_result = genome, result
            init_boost_frames, _ = start_boost_tier(result["boostCharge"]) if "boostCharge" in result else (None, None)
            init_progress = self._progress_of(result, progress_ref)
            self._set_status(iteration=0, bestFitness=best_fit, bestFinished=result["finished"],
                    bestTimeMs=result["timeMs"] if result["finished"] else None,
                    bestCompletion=result["completion"], bestSpeed=result.get("speed"),
                    bestProgress=init_progress,
                    referenceProgress=reference_progress,
                    progressDelta=(init_progress - reference_progress
                            if init_progress is not None and reference_progress is not None else None),
                    referenceCompletion=reference_completion,
                    completionDelta=(result["completion"] - reference_completion
                            if reference_completion is not None else None),
                    bestBoostCharge=result.get("boostCharge"), bestBoostFrames=init_boost_frames)

            workers = max(1, cfg.parallel_workers)
            executor = ThreadPoolExecutor(max_workers=workers) if workers > 1 else None
            iterations_since_improvement = 0
            try:
                for i in range(1, cfg.iterations + 1):
                    if self._stop.is_set():
                        break

                    if executor is not None:
                        # Each candidate is an independent mutation of the SAME current genome,
                        # evaluated concurrently -- separate kinoko_host subprocesses, so this is
                        # real parallel work despite the GIL (subprocess.run releases it while
                        # waiting on the child). Only the best of the batch is considered for this
                        # generation, same selection pressure as the single-candidate case.
                        #
                        # An earlier version double-evaluated every candidate to guard against a
                        # suspected flaky/non-reproducible engine result. That hypothesis turned out
                        # to be wrong -- the real cause of mismatched live/exported scores was a
                        # genuine bug (frames_from_genome scoring Intro-phase input that never
                        # survives into the exported .rkg, now fixed), not engine nondeterminism. A
                        # dedicated diagnostic run found zero disagreements between repeated
                        # evaluations of the same candidate once that bug was in play, so the
                        # confirmation step was pure overhead: it doubled every evaluation, which
                        # scaled with parallelism (more candidates per generation -> more frequent
                        # "improvements" to confirm) and fully erased the speedup parallel workers
                        # were supposed to provide.
                        # With a surrogate model active, propose a LARGER pool of candidates than
                        # real-evaluation slots (surrogate_oversample x), cheaply rank the whole
                        # pool by predicted progress (microseconds, no subprocess), and only spend
                        # real engine time on the most promising `workers` of them. The real
                        # fitness() call below still makes the actual accept/reject decision --
                        # the surrogate only decides what's worth checking in the first place, so a
                        # bad surrogate prediction can waste real evaluations on a dud but can never
                        # cause a genuinely bad candidate to be ACCEPTED.
                        pool_size = workers * cfg.surrogate_oversample if surrogate_model else workers
                        pool = [mutate(genome, cfg.mutation_rate, cfg.mutation_sigma,
                                boost_gene_index, boost_gene_sigma) for _ in range(pool_size)]
                        if surrogate_model:
                            import numpy as np
                            predicted = surrogate_model.predict(np.array(pool))
                            top_idx = np.argsort(predicted)[::-1][:workers]
                            candidates = [pool[int(idx)] for idx in top_idx]
                        else:
                            candidates = pool
                        results = list(executor.map(self._evaluate, candidates))
                        fits = [fitness(r, cfg.fitness_metric, progress_ref, cfg.boost_bonus_weight,
                                cfg.completion_bonus_weight, cfg.z_bonus_weight,
                                cfg.completion_tolerance, cfg.min_boost_charge,
                                cfg.speed_bonus_weight, cfg.max_boost_charge,
                                cfg.accel_hold_bonus_weight, cfg.heading_bonus_weight)
                                for r in results]
                        best_idx = min(range(len(fits)), key=lambda k: fits[k])
                        candidate, result, fit = candidates[best_idx], results[best_idx], fits[best_idx]
                    else:
                        candidate = mutate(genome, cfg.mutation_rate, cfg.mutation_sigma,
                                boost_gene_index, boost_gene_sigma)
                        result = self._evaluate(candidate)
                        fit = fitness(result, cfg.fitness_metric, progress_ref, cfg.boost_bonus_weight,
                                cfg.completion_bonus_weight, cfg.z_bonus_weight,
                                cfg.completion_tolerance, cfg.min_boost_charge,
                                cfg.speed_bonus_weight, cfg.max_boost_charge,
                                cfg.accel_hold_bonus_weight, cfg.heading_bonus_weight)

                    if fit < best_fit:
                        best_fit = fit
                        genome, self._best_genome, self._best_result = candidate, candidate, result
                        iterations_since_improvement = 0
                    else:
                        iterations_since_improvement += 1

                    ms_per_iteration = (time.time() - self._start_time) / i * 1000.0
                    eta_seconds = ms_per_iteration * (cfg.iterations - i) / 1000.0
                    boost_frames = None
                    if "boostCharge" in self._best_result:
                        boost_frames, _ = start_boost_tier(self._best_result["boostCharge"])
                    stagnant = (cfg.stagnation_stop_after is not None and
                            iterations_since_improvement >= cfg.stagnation_stop_after)
                    cur_progress = self._progress_of(self._best_result, progress_ref)
                    self._set_status(iteration=i, bestFitness=best_fit,
                            bestFinished=self._best_result["finished"],
                            bestTimeMs=self._best_result["timeMs"] if self._best_result["finished"] else None,
                            bestCompletion=self._best_result["completion"],
                            bestSpeed=self._best_result.get("speed"),
                            bestProgress=cur_progress,
                            referenceProgress=reference_progress,
                            progressDelta=(cur_progress - reference_progress
                                    if cur_progress is not None and reference_progress is not None
                                    else None),
                            referenceCompletion=reference_completion,
                            completionDelta=(self._best_result["completion"] - reference_completion
                                    if reference_completion is not None else None),
                            bestBoostCharge=self._best_result.get("boostCharge"),
                            bestBoostFrames=boost_frames,
                            iterationsSinceImprovement=iterations_since_improvement,
                            stagnant=stagnant,
                            msPerIteration=ms_per_iteration, etaSeconds=eta_seconds)

                    if stagnant:
                        # Heuristic "probably converged" auto-stop -- not a proof (this is a
                        # stochastic hill-climb, not exhaustive search), just a practical signal
                        # that spending more of the iteration budget here is unlikely to pay off.
                        break
            finally:
                if executor is not None:
                    executor.shutdown(wait=False, cancel_futures=True)
        except Exception as exc:  # noqa: BLE001 -- surface any failure to the GUI/CLI instead of dying silently
            self._set_status(error=str(exc))
        finally:
            self._set_status(running=False)


# ---------------------------------------------------------------------------
# .rkg export
#
# Encodes a winning frame sequence as a real ghost file, using the exact bit layouts
# GhostFile::read() (source/game/system/GhostFile.cc) and KPadGhostController::readGhostBuffer /
# KPadGhostButtonsStream (source/game/system/KPadController.cc) expect, so it round-trips through
# Kinoko's own reader by construction. The RKG format itself is documented at
# https://wiki.tockdom.com/wiki/RKG_(File_Format); fields Kinoko doesn't consult when reading an
# UNCOMPRESSED file (lap splits, Mii data/CRC, ghost "type") are written as zero/placeholder here
# -- verified against Kinoko, not independently checked against real hardware or Dolphin.
# ---------------------------------------------------------------------------

RKG_HEADER_SIZE = 0x88
RKG_INPUT_SECTION_SIZE = 0x2774
RKG_TOTAL_SIZE = 0x2800


def _crc16_xmodem(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _rle_encode(values, max_duration: int):
    tuples = []
    i, n = 0, len(values)
    while i < n:
        v = values[i]
        j = i + 1
        while j < n and values[j] == v and (j - i) < max_duration:
            j += 1
        tuples.append((v, j - i))
        i = j
    return tuples


def encode_rkg(course: int, character: int, vehicle: int, frames, finish_time_ms: int,
        lap_count: int = 3) -> bytes:
    """frames: the exact sequence evaluated by the engine (buttons, trick, stickX, stickY), indexed
    by ABSOLUTE engine frame (frame 0 = Intro start), matching what frames_from_genome produces and
    KBruteSystem consumes.

    A ghost's recorded stream is NOT read starting at absolute frame 0, though: Kinoko only starts
    consuming it once Countdown begins (RaceManager::calc() -> KPadDirector::startGhostProxies() at
    COUNTDOWN_START_FRAME -- see decode_rkg's docstring for how this was verified against the real
    engine). So frames[:COUNTDOWN_START_FRAME] (Intro, where input has no effect either way) are
    dropped here rather than encoded -- encoding them as-is would shift every recorded input
    COUNTDOWN_START_FRAME frames too late once the file is loaded as an actual ghost, silently
    turning a validated attempt into a different, unvalidated one.
    """
    frames = list(frames)[COUNTDOWN_START_FRAME:]

    face_values = [buttons & 0xF for buttons, _trick, _x, _y in frames]
    dir_values = [(_stick_raw(x) << 4) | _stick_raw(y) for _buttons, _trick, x, y in frames]
    trick_values = [trick & 0xF for _buttons, trick, _x, _y in frames]

    face_tuples = _rle_encode(face_values, 255)
    dir_tuples = _rle_encode(dir_values, 255)
    trick_tuples = _rle_encode(trick_values, 4095)

    input_body = bytearray()
    input_body += struct.pack(">HHH", len(face_tuples), len(dir_tuples), len(trick_tuples))
    input_body += b"\x00\x00"  # matches readGhostBuffer's stream.skip(2)
    for value, duration in face_tuples:
        input_body += struct.pack(">BB", value, duration)
    for value, duration in dir_tuples:
        input_body += struct.pack(">BB", value, duration)
    for trick_type, duration in trick_tuples:
        high = ((trick_type & 0xF) << 4) | ((duration >> 8) & 0xF)
        low = duration & 0xFF
        input_body += struct.pack(">BB", high, low)

    used_input_bytes = len(input_body)
    if used_input_bytes > RKG_INPUT_SECTION_SIZE:
        raise ValueError(
                f"encoded input ({used_input_bytes} bytes) exceeds the RKG input section "
                f"({RKG_INPUT_SECTION_SIZE} bytes) -- fewer control points or a shorter run "
                "needed")
    input_body += b"\x00" * (RKG_INPUT_SECTION_SIZE - used_input_bytes)

    min_, rem = divmod(max(finish_time_ms, 0), 60000)
    sec, mil = divmod(rem, 1000)

    word1 = ((min_ & 0x7F) << 25) | ((sec & 0x7F) << 18) | ((mil & 0x3FF) << 8) | ((course & 0x3F) << 2)
    # year=1/month=1/day=1 (relative to 2000): an arbitrary but valid date, RawGhostFile::isValid
    # only rejects year>=100/day>=32/month>12, it doesn't check these against anything real.
    word2 = ((vehicle & 0x3F) << 26) | ((character & 0x3F) << 20) | (1 << 13) | (1 << 9) | (1 << 4)
    # ghost type 0, manual drift (matches OnInit's player.driftIsAuto=false), inputSize is only
    # consulted when decompressing -- we write uncompressed data, so this is documentation only.
    word3 = (used_input_bytes & 0xFFFF)

    header = bytearray(RKG_HEADER_SIZE)
    header[0:4] = b"RKGD"
    header[4:8] = struct.pack(">I", word1)
    header[8:12] = struct.pack(">I", word2)
    header[12:16] = struct.pack(">I", word3)
    header[16] = lap_count
    # 0x11-0x1F: 5 lap splits, 3 bytes each -- not read back by Kinoko for physics, left zero.
    header[0x34] = 0xFF  # country: sharing disabled
    header[0x35] = 0xFF  # status: sharing disabled
    header[0x36:0x38] = b"\xFF\xFF"  # location: sharing disabled
    # 0x38-0x3B unknown, 0x3C-0x85 Mii data: left zero.
    crc = _crc16_xmodem(bytes(header[0x3C:0x86]))
    header[0x86:0x88] = struct.pack(">H", crc)

    ghost = bytes(header) + bytes(input_body)
    ghost += b"\x00" * (RKG_TOTAL_SIZE - len(ghost))
    return ghost


def _rle_decode_frames(data: bytes, count: int, trick: bool = False):
    """Inverse of _rle_encode's packing. Expands `count` (value, duration) tuples, each 2 bytes,
    into a flat per-frame list of raw values (see KPadGhostButtonsStream::readFrame)."""
    values = []
    for i in range(count):
        high, low = data[2 * i], data[2 * i + 1]
        if trick:
            value = high >> 4
            duration = ((high & 0xF) << 8) | low
        else:
            value = high
            duration = low
        values.extend([value] * duration)
    return values


def _yaz_decompress(data: bytes) -> bytes:
    """Standard Nintendo Yaz0-family LZ77 decompressor -- also handles the "Yaz1" magic real RKG
    ghost files use for their compressed input section (identical algorithm, just a different magic
    word; confirmed by decompressing a real compressed community ghost and getting a byte stream
    that parses cleanly as the same face/dir/trick RLE-count header _rle_decode_frames expects).
    Header: magic(4) + decompressed size(4, big-endian) + reserved(8), then the LZ-coded payload."""
    if data[:4] not in (b"Yaz0", b"Yaz1"):
        raise ValueError(f"not a Yaz0/Yaz1 buffer (magic: {data[:4]!r})")
    size = struct.unpack_from(">I", data, 4)[0]
    src, dst = 16, bytearray()
    while len(dst) < size:
        flags = data[src]
        src += 1
        for bit in range(8):
            if len(dst) >= size:
                break
            if flags & (0x80 >> bit):
                dst.append(data[src])
                src += 1
                continue
            b1, b2 = data[src], data[src + 1]
            src += 2
            dist = ((b1 & 0xF) << 8 | b2) + 1
            n = b1 >> 4
            if n == 0:
                n = data[src] + 0x12
                src += 1
            else:
                n += 2
            start = len(dst) - dist
            if dist >= n:
                dst += dst[start:start + n]
            else:
                for i in range(n):
                    dst.append(dst[start + i])
    return bytes(dst)


def decode_rkg(path: str):
    """Inverse of encode_rkg: reads course/character/vehicle and the per-frame input sequence back
    out of an RKG file. Handles both this project's own uncompressed output AND real compressed
    community ghosts (per wiki.tockdom.com/wiki/RKG_(File_Format)): a compressed file's input
    section, starting at RKG_HEADER_SIZE, is a u32 blob length followed by a Yaz1-magic'd LZ77 blob
    (see _yaz_decompress) of exactly that length, then a trailing 4-byte CRC-32 -- once decompressed,
    the result has the exact same face/dir/trick RLE-count layout as the uncompressed case, just
    starting at offset 0 of the decompressed buffer instead of RKG_HEADER_SIZE of the file.

    Returns (course, character, vehicle, frames) where frames is a list of (buttons, trick, stickX,
    stickY), ALIGNED to the engine's own absolute frame numbering (frame 0 = Intro start, matching
    KBruteSystem/frames_from_genome) -- NOT starting at the ghost file's own recorded frame 0.

    This matters because a ghost's recorded input stream isn't actually read from frame 0: Kinoko's
    KPadGhostController::calcImpl() (source/game/system/KPadController.cc) does nothing at all until
    RaceManager::calc() calls startGhostProxies() when Countdown begins, at COUNTDOWN_START_FRAME
    (verified against the real engine in the browser: a real ghost's own recorded tuple stream,
    read starting at absolute frame 0, produced a kart that never got moving -- while offsetting it
    by COUNTDOWN_START_FRAME reproduced the real trajectory almost exactly). So the recorded tuples'
    OWN frame 0 corresponds to absolute frame COUNTDOWN_START_FRAME, not engine frame 0; frames
    before that are Intro, where input is ignored anyway, so they're padded with neutral input.
    """
    with open(path, "rb") as f:
        data = f.read()

    if data[0:4] != b"RKGD":
        raise ValueError(f"{path} is not an RKG file (missing RKGD magic)")

    word1 = struct.unpack(">I", data[4:8])[0]
    course = (word1 >> 2) & 0x3F

    word2 = struct.unpack(">I", data[8:12])[0]
    vehicle = word2 >> 26
    character = (word2 >> 20) & 0x3F

    if (data[0xC] >> 3) & 1:
        blob_len = struct.unpack_from(">I", data, RKG_HEADER_SIZE)[0]
        blob_start = RKG_HEADER_SIZE + 4
        body = _yaz_decompress(data[blob_start:blob_start + blob_len])
        off = 0
    else:
        body = data
        off = RKG_HEADER_SIZE

    face_count, dir_count, trick_count = struct.unpack(">HHH", body[off:off + 6])
    off += 8  # 3 counts + 2 bytes skipped (matches readGhostBuffer)

    face_values = _rle_decode_frames(body[off:], face_count)
    off += face_count * 2
    dir_values = _rle_decode_frames(body[off:], dir_count)
    off += dir_count * 2
    trick_values = _rle_decode_frames(body[off:], trick_count, trick=True)

    n = max(len(face_values), len(dir_values), len(trick_values))
    frames = [(0, 0, 0.0, 0.0)] * COUNTDOWN_START_FRAME  # Intro: ghost stream isn't read yet
    for i in range(n):
        buttons = face_values[i] if i < len(face_values) else 0
        dir_byte = dir_values[i] if i < len(dir_values) else 0x77  # neutral (raw 7,7)
        trick = trick_values[i] if i < len(trick_values) else 0
        stick_x = ((dir_byte >> 4) - 7) / 7.0
        stick_y = ((dir_byte & 0xF) - 7) / 7.0
        frames.append((buttons, trick, stick_x, stick_y))

    return course, character, vehicle, frames


def prefix_frames_from_rkg(path: str, frame_count: Optional[int] = None) -> list:
    """Reads a previous segment's WINNING .rkg and returns its frame sequence for use as a locked
    prefix in segment-chained search (see frames_from_genome's prefix_frames param) -- these frames
    are replayed verbatim every evaluation, never mutated. frame_count caps how many frames to take
    (e.g. if path holds more than one already-optimized segment and you only want the first);
    None (default) takes the whole recorded sequence. Raises if frame_count exceeds what's recorded."""
    _course, _character, _vehicle, frames = decode_rkg(path)
    if frame_count is None:
        return frames
    if frame_count > len(frames):
        raise ValueError(f"{path} only has {len(frames)} recorded frames, need {frame_count}")
    return frames[:frame_count]


def genome_from_frames(frames, step: int, n_points: int, search_start_boost: bool = False,
        search_accelerate: bool = False, search_trick: bool = False) -> list:
    """Samples stickX from a decoded frame sequence at each control-point frame index -- the
    inverse of frames_from_genome, used to seed a search from an existing attempt (e.g. a prior
    best.rkg) instead of starting from random noise.

    With search_accelerate, also appends the accelerate channel, sampled the same way as steering
    (accelerate-held at that frame -> 1.0, else -1.0) -- this DOES faithfully carry over a real
    ghost's multi-tap accelerate pattern, since the channel can represent arbitrary on/off timing.

    With search_start_boost (and not search_accelerate), instead appends the single accel-hold-
    start-frame gene, read back from wherever the decoded frames first hold accelerate -- which
    only makes sense as a seed if the seed itself used a single clean hold, matching this genome's
    own "hold continuously from here" model. A real ghost's own recording can tap accelerate on and
    off several times while charging (a valid technique the single-gene model can't represent at
    all), and naively seeding from its first press would then hand the search a hold duration that
    burns out -- exactly the failure this whole feature exists to avoid. So: if holding
    continuously from that first press would burn out, this falls back to the analytically correct
    frame instead (see find_best_start_boost_frame), the same seed random_genome uses when there's
    no file to read at all -- the search still starts from a genuinely non-burnt-out attempt.

    With search_trick, also appends the Trick::Up channel, sampled the same way as accelerate."""
    genome = []
    for i in range(n_points):
        idx = min(i * step, len(frames) - 1) if frames else 0
        genome.append(frames[idx][2] if frames else 0.0)

    if search_accelerate:
        for i in range(n_points):
            idx = min(i * step, len(frames) - 1) if frames else 0
            held = frames[idx][0] & BUTTON_ACCELERATE if frames else 0
            genome.append(1.0 if held else -1.0)
    elif search_start_boost:
        accel_start_frame = COUNTDOWN_START_FRAME
        for i, (buttons, _trick, _x, _y) in enumerate(frames):
            if buttons & BUTTON_ACCELERATE:
                accel_start_frame = i
                break

        _boost_frames, burned_out = start_boost_tier(start_boost_charge(accel_start_frame))
        if burned_out:
            accel_start_frame, _charge, _boost = find_best_start_boost_frame()

        frac = (accel_start_frame - COUNTDOWN_START_FRAME) / (GO_FRAME - COUNTDOWN_START_FRAME)
        genome.append(max(0.0, min(1.0, frac)))

    if search_trick:
        for i in range(n_points):
            idx = min(i * step, len(frames) - 1) if frames else 0
            tricking = frames[idx][1] == System_Trick_Up if frames else False
            genome.append(1.0 if tricking else -1.0)

    return genome


def genome_from_rkg(path: str, step: int, n_points: int, search_start_boost: bool = False,
        search_accelerate: bool = False, search_trick: bool = False, start_frame: int = 0) -> list:
    """start_frame: segment-chaining -- when seeding a search whose genome covers a window starting
    partway through the race (SearchConfig.prefix_rkg), pass the SAME frame index used as that
    prefix's length here too, so the sampled genome lines up with frames_from_genome's own
    segment_start instead of sampling from the wrong point in the file. 0 (default) = old behavior
    (genome covers from the file's own frame 0, i.e. a normal single-segment seed)."""
    _course, _character, _vehicle, frames = decode_rkg(path)
    return genome_from_frames(frames[start_frame:], step, n_points, search_start_boost,
            search_accelerate, search_trick)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser(description: str = __doc__) -> argparse.ArgumentParser:
    """The CLI's arguments, shared with tools/gpu_brute.py (which adds its own GPU-only ones)."""
    parser = argparse.ArgumentParser(description=description,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True, help="path to the kinoko_host executable")
    parser.add_argument("--course", type=int, required=True)
    parser.add_argument("--character", type=int, required=True)
    parser.add_argument("--vehicle", type=int, required=True)
    parser.add_argument("--iterations", type=int, default=500)
    parser.add_argument("--n-points", type=int, default=24, help="number of steering control points")
    parser.add_argument("--step", type=int, default=15,
            help="frames between control points -- use a small value (e.g. 1-3) for a "
                    "frame-critical short window like a start slide")
    parser.add_argument("--max-frames", type=int, default=10800,
            help="frame cap for the whole attempt -- for a start-slide/start-boost search, set "
                    "this to ~650-750 (the countdown alone takes 411 frames, and completion/speed "
                    "don't show real differences until ~550, verified empirically) rather than a "
                    "full race")
    parser.add_argument("--mutation-rate", type=float, default=0.3)
    parser.add_argument("--mutation-sigma", type=float, default=0.15,
            help="mutation step size -- use a smaller value (e.g. 0.03-0.05) when refining an "
                    "already-good --seed-rkg instead of searching from scratch")
    parser.add_argument("--seed-rkg",
            help="an existing .rkg (e.g. a previous best.rkg) to seed the search from, instead of "
                    "starting from random noise -- decoded with the SAME --step/--n-points so the "
                    "sampled control points line up with this run's genome. With --prefix-rkg set, "
                    "this should be a combined (prefix+tail) result for the SAME segment you're "
                    "re-refining -- sampling starts at --prefix-frame-count so it lines up with the "
                    "free tail window, not the locked prefix")
    parser.add_argument("--fitness-metric", choices=["completion", "speed", "progress"],
            default="completion",
            help="'completion' (default) for a full lap/race search; 'speed' for a short window "
                    "like a start slide, where completion stays flat until the next checkpoint; "
                    "'progress' to score against how far a --reference-rkg itself travelled, "
                    "rewarding progress along that SAME line rather than just speed in any "
                    "direction")
    parser.add_argument("--reference-rkg",
            help="required with --fitness-metric progress: the ghost whose own displacement "
                    "(from --progress-start-frame to --max-frames) defines the target direction")
    parser.add_argument("--progress-start-frame", type=int, default=COUNTDOWN_START_FRAME,
            help="where the reference ghost's displacement is measured from (default: Countdown "
                    "start, since the kart can't move under its own power before then anyway)")
    parser.add_argument("--search-start-boost", action="store_true",
            help="add the single accel-hold-start-frame gene so a start boost is reachable at all "
                    "-- without this, accelerate is always held for the whole countdown, which "
                    "guarantees a burned-out start (charge overshoots 0.95) every single run. "
                    "Ignored if --search-accelerate is also set")
    parser.add_argument("--search-accelerate", action="store_true",
            help="add a full accelerate on/off channel (one gene per steering control point) "
                    "instead of the single start-boost gene -- needed to represent a real "
                    "multi-tap accelerate pattern (e.g. one that also times a drift/hop into a "
                    "corner), which the single-gene model can't reproduce at all. Bigger search "
                    "space, takes longer to converge than --search-start-boost")
    parser.add_argument("--search-trick", action="store_true",
            help="add a Trick::Up (wheelie) on/off channel, same shape as --search-accelerate -- "
                    "verified necessary for at least one real reference ghost, whose countdown "
                    "movement couldn't be reproduced by steering+accelerate alone but matched "
                    "exactly once its real wheelie taps were carried over")
    parser.add_argument("--out", default="best.rkg")
    parser.add_argument("--parallel-workers", type=int, default=1,
            help="evaluate this many mutated candidates concurrently per generation and keep the "
                    "best of the batch -- each is an independent kinoko_host subprocess, so this "
                    "roughly multiplies throughput by core count. 1 = old single-candidate behavior")
    parser.add_argument("--boost-bonus-weight", type=float, default=0.0,
            help="explicit reward for start-boost quality, using the engine's real boostCharge -- "
                    "see fitness()'s docstring. 0.0 = no boost pressure (old behavior). Needed "
                    "because a short max_frames window can't see the boosted speed that follows GO, "
                    "so without this the search has zero incentive to find a genuine full boost")
    parser.add_argument("--min-boost-charge", type=float, default=None,
            help="hard gate: reject any candidate whose boostCharge falls below this -- see "
                    "fitness()'s docstring. Use 0.94 to forbid ever dropping out of the 70-boost-"
                    "frame tier. None (default) = no gate (old behavior)")
    parser.add_argument("--speed-bonus-weight", type=float, default=0.0,
            help="explicit reward for raw forward speed at the final scored frame -- see "
                    "fitness()'s docstring. Without this, a candidate can score well on cumulative "
                    "position/completion while ending mid-spin/stalled. 0.0 (default) = no speed "
                    "pressure at the cutoff (old behavior)")
    parser.add_argument("--max-boost-charge", type=float, default=None,
            help="hard gate, mirror of --min-boost-charge: reject any candidate whose boostCharge "
                    "EXCEEDS this. Use 0.95 to forbid ever crossing into full burnout (0 boost "
                    "frames). None (default) = no gate (old behavior)")
    parser.add_argument("--accel-hold-bonus-weight", type=float, default=0.0,
            help="reward for total Countdown-phase frames spent holding accelerate, independent of "
                    "the resulting charge tier -- see fitness()'s docstring. Pair with "
                    "--max-boost-charge 0.95 so the search can push hold-time up without burning out "
                    "the charge. 0.0 (default) = old behavior")
    parser.add_argument("--ternary-steering", action="store_true",
            help="constrain steering to only full-left/neutral/full-right (-1, 0, 1) instead of the "
                    "full 15-position grid -- see _quantize_stick_ternary's docstring")
    parser.add_argument("--heading-checkpoint-frame", type=int, default=None,
            help="frame at which to score how far the kart has turned toward the reference's own "
                    "turn direction -- see compute_heading_reference and fitness()'s "
                    "heading_bonus_weight docstring. Requires --reference-rkg")
    parser.add_argument("--heading-bonus-weight", type=float, default=0.0,
            help="reward for turning toward the reference's own direction by "
                    "--heading-checkpoint-frame. 0.0 (default) = no early-rotation pressure")
    parser.add_argument("--stagnation-stop-after", type=int, default=None,
            help="stop early once this many generations pass with no accepted improvement -- a "
                    "heuristic 'probably converged' signal, not a proof. Unset = always run the "
                    "full --iterations count regardless of stagnation")
    parser.add_argument("--prefix-rkg", default=None,
            help="segment-chaining: a previous segment's WINNING .rkg, replayed verbatim as a "
                    "locked prefix -- this run's genome only searches the NEW frames from where it "
                    "ends through --max-frames. Remember to also set --progress-start-frame to the "
                    "prefix's length so fitness_metric=progress scores only the new segment. None "
                    "(default) = old single-segment behavior")
    parser.add_argument("--prefix-frame-count", type=int, default=None,
            help="how many frames of --prefix-rkg to lock; None (default) takes the whole file")
    parser.add_argument("--completion-bonus-weight", type=float, default=0.0,
            help="continuous reward for exceeding the reference's own raceCompletion, on top of the "
                    "hard gate that's always active for fitness_metric='progress'. 0.0 = gate only "
                    "(a floor against falling behind), no pull toward actually pulling ahead on the "
                    "track's own curve -- see fitness()'s docstring")
    parser.add_argument("--z-bonus-weight", type=float, default=0.0,
            help="explicit EXTRA reward for Z-axis progress specifically (in the reference's own Z "
                    "direction), on top of the piecewise polyline score, not instead of it. 0.0 = "
                    "old behavior (no extra Z pull) -- see fitness()'s docstring")
    parser.add_argument("--completion-tolerance", type=int, default=1,
            help="rounding margin on the completion gate, since raceCompletion is coarsely "
                    "quantized -- 0 requires an exact >= match against the reference, which can "
                    "reject over half of all mutations from an already-good seed purely on "
                    "quantization noise, not real regressions. 1 (default) allows a one-unit margin")
    parser.add_argument("--eval-log-path", default=None,
            help="append a JSONL record for every real evaluation (genome, progress, completion, "
                    "boostCharge, config metadata) -- feeds tools/train_surrogate.py. None "
                    "(default) = no logging")
    parser.add_argument("--surrogate-path", default=None,
            help="a trained surrogate model (see tools/train_surrogate.py) used to cheaply "
                    "pre-rank a larger pool of candidates per generation, spending real engine "
                    "evaluation only on the most promising subset -- requires "
                    "fitness_metric='progress' and --parallel-workers > 1. None (default) = off")
    parser.add_argument("--surrogate-oversample", type=int, default=4,
            help="candidates proposed per real-evaluation slot when a surrogate is active "
                    "(e.g. workers=8, oversample=4 -> 32 proposed, surrogate-ranked, top 8 "
                    "real-evaluated)")
    return parser


def config_from_args(args) -> SearchConfig:
    seed_genome = None
    if args.seed_rkg:
        seed_genome = genome_from_rkg(args.seed_rkg, args.step, args.n_points,
                args.search_start_boost, args.search_accelerate, args.search_trick,
                start_frame=args.prefix_frame_count or 0)

    cfg = SearchConfig(binary=args.binary, course=args.course, character=args.character,
            vehicle=args.vehicle, iterations=args.iterations, n_points=args.n_points,
            step=args.step, max_frames=args.max_frames, mutation_rate=args.mutation_rate,
            mutation_sigma=args.mutation_sigma, seed_genome=seed_genome,
            fitness_metric=args.fitness_metric, search_start_boost=args.search_start_boost,
            search_accelerate=args.search_accelerate, search_trick=args.search_trick,
            reference_rkg=args.reference_rkg, progress_start_frame=args.progress_start_frame,
            parallel_workers=args.parallel_workers, boost_bonus_weight=args.boost_bonus_weight,
            min_boost_charge=args.min_boost_charge, speed_bonus_weight=args.speed_bonus_weight,
            max_boost_charge=args.max_boost_charge,
            accel_hold_bonus_weight=args.accel_hold_bonus_weight,
            ternary_steering=args.ternary_steering,
            heading_checkpoint_frame=args.heading_checkpoint_frame,
            heading_bonus_weight=args.heading_bonus_weight,
            stagnation_stop_after=args.stagnation_stop_after,
            completion_bonus_weight=args.completion_bonus_weight, z_bonus_weight=args.z_bonus_weight,
            completion_tolerance=args.completion_tolerance, eval_log_path=args.eval_log_path,
            surrogate_path=args.surrogate_path, surrogate_oversample=args.surrogate_oversample,
            prefix_rkg=args.prefix_rkg, prefix_frame_count=args.prefix_frame_count)
    return cfg


def main():
    args = build_parser().parse_args()
    cfg = config_from_args(args)

    def report(status):
        if status["bestFitness"] is None:
            return
        if status["iteration"] % 10 == 0 or not status["running"]:
            if status["bestFinished"]:
                best = "%.0fms" % status["bestTimeMs"]
            elif cfg.fitness_metric == "speed":
                best = "%.3f speed" % status["bestSpeed"]
            elif cfg.fitness_metric == "progress":
                best = "%.2f units of progress" % status["bestProgress"]
            else:
                best = str(status["bestCompletion"]) + "/10000-per-lap completion"
            pace = ""
            if status["msPerIteration"] is not None:
                pace = f" ({status['msPerIteration']:.0f} ms/iter, ~{status['etaSeconds']:.0f}s left)"
            print(f"iter {status['iteration']}/{status['totalIterations']} best={best}{pace}")

    search = BruteForceSearch(cfg, on_status=report)
    search.start()
    try:
        search.join()
    except KeyboardInterrupt:
        search.stop()
        search.join()

    genome, result = search.best()
    if not genome:
        status = search.status()
        print(f"no result found: {status['error'] or 'unknown error'}", file=sys.stderr)
        sys.exit(1)

    frames = list(frames_from_genome(genome, cfg.step, cfg.max_frames, cfg.search_start_boost,
            cfg.search_accelerate, cfg.search_trick, cfg.n_points, search.locked_prefix_frames(),
            cfg.ternary_steering))
    rkg = encode_rkg(cfg.course, cfg.character, cfg.vehicle, frames,
            result["timeMs"] if result["finished"] else 0)
    with open(args.out, "wb") as f:
        f.write(rkg)
    print(f"wrote {args.out} ({'finished ' + str(result['timeMs']) + 'ms' if result['finished'] else 'did not finish'})")


if __name__ == "__main__":
    main()
