#!/usr/bin/env python3
"""Beam search over Kinoko races, with a ghost-trained policy as a prior.

A node is a saved simulation state (Kinoko's Host::Context snapshot, ~4 ms to restore) plus the inputs
that led to it. All nodes of a step are at the same frame. Each step expands every node by one segment of
`--seg` frames in several ways, scores the children by track progress at that frame, and keeps the best:

    policy       the policy drives the segment greedily (argmax)
    policy_T     the policy drives with sampled inputs (two temperatures)
    forced       stick forced left / right / neutral for the first frames, then the policy takes over
    ghost        a ghost's own next segment (only for a node still exactly on that ghost's line)
    ghost_mut    that ghost segment with a few stick values changed

The best ghosts' lines (`--top-ghosts`) are protected, so the search can never end up slower than the best
seed: it either keeps their line or replaces it with something that reached further. The policy is the
prior that decides what the non-ghost children try; the output records which kind of child each segment of
the winning run came from, which is the honest measure of whether the prior helps.

    python tools/beam_search.py --ghosts samples/tracks/luigi-circuit/ghosts/lc-top100-pb --policy samples/data/dumps/lc_policy_path.npz \
        --out samples/tracks/luigi-circuit/runs/beam/best.rkg
"""

from __future__ import annotations

import argparse
import multiprocessing as mp
import os
import sys
import time
from collections import Counter

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from brute_force import GO_FRAME, decode_rkg, encode_rkg, find_best_start_boost_frame  # noqa: E402
from kinoko_search import DEFAULT_BINARY, KinokoSearch  # noqa: E402
from policy_net import Policy, state_features  # noqa: E402
from prepare_ghosts import parse_rkg  # noqa: E402

NEUTRAL = (0, 0, 0.0, 0.0)  # (buttons, trick, stickX, stickY): the order encode_rkg uses
SPEED_BONUS = 2e-4          # progress value of one unit of speed: roughly 30 frames of travel
MUSHROOM_BONUS = 0.02       # progress value of a mushroom still in hand (set by --mushroom-bonus)


def snap(v):
    return round(float(v) * 7) / 7


# ---------------------------------------------------------------------------------------------------------
# Worker: owns one `kinoko_host search` process and the snapshots of the nodes that live in it.
# ---------------------------------------------------------------------------------------------------------

def run_segment(race, pol, row, spec, length, cols):
    """Simulate up to `length` frames from the current state according to `spec`.

    Returns (inputs, last row, END dict or None)."""
    kind = spec[0]
    rng = None
    temperature = 0.0
    forced, hold = None, 0
    if kind == "policy":
        _, temperature, seed = spec
        rng = np.random.default_rng(seed) if temperature > 0 else None
    elif kind == "forced":
        _, forced, hold, temperature, seed = spec
        rng = np.random.default_rng(seed) if temperature > 0 else None
    fixed = spec[1] if kind == "ghost" else None

    inputs = []
    for i in range(length):
        if fixed is not None:
            inp = fixed[i] if i < len(fixed) else NEUTRAL
        else:
            feats, _ = state_features(row[None, :], cols, False)
            a = pol.act(feats, rng, temperature)
            sx = forced if (kind == "forced" and i < hold) else snap(a["stickX"][0])
            inp = (int(a["buttons"][0]), int(a["trick"][0]), sx, snap(a["stickY"][0]))
        row = race.step(inp[0], inp[2], inp[3], inp[1])
        inputs.append(inp)
        if race.end is not None:
            break
    return inputs, row, race.end


def worker_main(conn, binary, policy_path, course, character, vehicle):
    race = KinokoSearch(binary, course, character, vehicle)
    pol = Policy.load(policy_path)
    cols = race.cols
    race.save(0)  # frame 0, so any lineage can be replayed from scratch in this process
    conn.send(("ready", cols))
    while True:
        msg = conn.recv()
        cmd = msg[0]
        if cmd == "replay":  # ("replay", inputs, new_id): play inputs from frame 0, snapshot the result
            _, inputs, new_id = msg
            race.load(0)
            row = None
            for inp in inputs:
                row = race.step(inp[0], inp[2], inp[3], inp[1])
                if race.end is not None:
                    break
            race.save(new_id)
            conn.send((row, race.end))
        elif cmd == "expand":  # ("expand", parent_id, parent_row, specs, length)
            _, parent_id, parent_row, specs, length = msg
            out = []
            for spec in specs:
                race.load(parent_id)
                out.append(run_segment(race, pol, parent_row, spec, length, cols))
            conn.send(out)
        elif cmd == "materialize":  # ("materialize", parent_id, inputs, new_id)
            _, parent_id, inputs, new_id = msg
            race.load(parent_id)
            row = None
            for inp in inputs:
                row = race.step(inp[0], inp[2], inp[3], inp[1])
            race.save(new_id)
            conn.send(row)
        elif cmd == "free":
            for i in msg[1]:
                race.free(i)
            conn.send("ok")
        elif cmd == "verify":  # ("verify", inputs): play from frame 0 and report the END line
            _, inputs = msg
            race.load(0)
            for inp in inputs:
                race.step(inp[0], inp[2], inp[3], inp[1])
                if race.end is not None:
                    break
            conn.send(race.end)
        elif cmd == "quit":
            race.close()
            return


class Pool:
    def __init__(self, n, binary, policy_path, course, character, vehicle):
        # Keep each worker's BLAS single-threaded: with one thread per core every worker reserves ~1.6 GB.
        for var in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"):
            os.environ[var] = "1"
        ctx = mp.get_context("spawn")
        self.conns, self.procs = [], []
        for _ in range(n):
            parent, child = ctx.Pipe()
            p = ctx.Process(target=worker_main, args=(child, binary, policy_path, course, character, vehicle), daemon=True)
            p.start()
            self.conns.append(parent)
            self.procs.append(p)
        self.cols = None
        for c in self.conns:
            tag, cols = c.recv()
            assert tag == "ready"
            self.cols = cols

    def call_all(self, msgs):
        """msgs: {worker index: message}. Sends everything, then collects, so workers run in parallel."""
        for w, m in msgs.items():
            self.conns[w].send(m)
        return {w: self.conns[w].recv() for w in msgs}

    def close(self):
        for c in self.conns:
            try:
                c.send(("quit",))
            except OSError:
                pass
        for p in self.procs:
            p.join(timeout=10)


# ---------------------------------------------------------------------------------------------------------
# Search
# ---------------------------------------------------------------------------------------------------------

class Node:
    __slots__ = ("id", "worker", "frame", "row", "inputs", "lineage", "value", "kinds")

    def __init__(self, id, worker, frame, row, inputs, lineage, kinds):
        self.id, self.worker, self.frame, self.row = id, worker, frame, row
        self.inputs, self.lineage, self.kinds = inputs, lineage, kinds
        self.value = 0.0


def node_value(row, cols):
    return (row[cols.index("completion")] + SPEED_BONUS * row[cols.index("speed")]
            + MUSHROOM_BONUS * row[cols.index("mushrooms")])


def mutate(segment, rng, stick_steps=(-3, -2, -1, 1, 2, 3)):
    """A few short stick changes inside a ghost segment."""
    seg = list(segment)
    if not seg:
        return seg
    for _ in range(int(rng.integers(1, 4))):
        start = int(rng.integers(0, len(seg)))
        run = int(rng.integers(1, 6))
        delta = int(rng.choice(stick_steps))
        for i in range(start, min(start + run, len(seg))):
            b, t, sx, sy = seg[i]
            seg[i] = (b, t, snap(max(-1.0, min(1.0, sx + delta / 7.0))), sy)
    return seg


def load_ghosts(folder, count, offset=0):
    files = sorted(f for f in os.listdir(folder) if f.lower().endswith(".rkg"))[offset:offset + count]
    ghosts = []
    for f in files:
        path = os.path.join(folder, f)
        header = parse_rkg(open(path, "rb").read())
        ghosts.append(dict(name=f, frames=decode_rkg(path)[3], time_ms=header["timeMs"]))
    return ghosts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ghosts", required=True, help="folder of .rkg ghosts, fastest first by file name")
    ap.add_argument("--policy", required=True)
    ap.add_argument("--out", required=True, help="where to write the best run as an .rkg")
    ap.add_argument("--top-ghosts", type=int, default=3, help="ghost lines that seed and are protected in the beam")
    ap.add_argument("--ghost-offset", type=int, default=0, help="skip this many of the fastest ghosts (to test with weaker seeds)")
    ap.add_argument("--beam", type=int, default=16)
    ap.add_argument("--seg", type=int, default=20, help="frames per search step")
    ap.add_argument("--max-per-parent", type=int, default=4)
    ap.add_argument("--mutations", type=int, default=2, help="mutated ghost children per ghost-lineage node")
    ap.add_argument("--workers", type=int, default=10)
    ap.add_argument("--max-steps", type=int, default=100000, help="stop after this many steps (for testing)")
    ap.add_argument("--save-top", type=int, default=0, help="also write this many of the fastest distinct finished runs")
    ap.add_argument("--top-dir", help="folder for --save-top (.rkg files named by finish time)")
    ap.add_argument("--no-policy", action="store_true", help="ablation: only ghost and mutated-ghost children")
    ap.add_argument("--mushroom-bonus", type=float, default=0.02,
            help="progress value of a mushroom still in hand; 0 reproduces the myopic score")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--binary", default=DEFAULT_BINARY)
    ap.add_argument("--course", type=int, default=8)
    ap.add_argument("--character", type=int, default=22)
    ap.add_argument("--vehicle", type=int, default=32)
    args = ap.parse_args()

    global MUSHROOM_BONUS
    MUSHROOM_BONUS = args.mushroom_bonus
    rng = np.random.default_rng(args.seed)
    ghosts = load_ghosts(args.ghosts, args.top_ghosts, args.ghost_offset)
    start_frame = find_best_start_boost_frame()[0]
    pool = Pool(args.workers, args.binary, args.policy, args.course, args.character, args.vehicle)
    cols = pool.cols
    ci, si = cols.index("completion"), cols.index("speed")
    # Snapshot ids index a fixed table in the engine (1024 slots, 0 is the frame-0 root), so freed ids are reused.
    free_ids = list(range(1023, 0, -1))

    def new_id():
        return free_ids.pop()

    # --- Seeds: the scripted-start node (no lineage) and one node per protected ghost line ---------------
    scripted = [(1 if f >= start_frame else 0, 0, 0.0, 0.0) for f in range(GO_FRAME)]
    seeds = [(None, scripted)] + [(g, ghosts[g]["frames"][:GO_FRAME]) for g in range(len(ghosts))]
    nodes = []
    jobs = {}
    for k, (lin, inputs) in enumerate(seeds):
        w = k % args.workers
        jobs.setdefault(w, []).append((k, lin, inputs))
    for w, lst in jobs.items():
        for k, lin, inputs in lst:  # one at a time per worker (replays are cheap)
            nid = new_id()
            row, end = pool.call_all({w: ("replay", inputs, nid)})[w]
            nodes.append(Node(nid, w, len(inputs), row, list(inputs), lin, []))
    for n in nodes:
        n.value = node_value(n.row, cols)
    print(f"seeded {len(nodes)} nodes at frame {nodes[0].frame}; ghosts: "
          + ", ".join(f"{g['time_ms'] / 1000:.3f}s" for g in ghosts), flush=True)

    # Sanity check that decode_rkg inputs reproduce each ghost's recorded finish time in the search engine.
    for g_i, g in enumerate(ghosts):
        end = pool.call_all({0: ("verify", g["frames"])})[0]
        ok = end is not None and abs(end["timeMs"] - g["time_ms"]) <= 1
        print(f"  ghost {g_i}: header {g['time_ms'] / 1000:.3f}s, replayed "
              f"{end['timeMs'] / 1000 if end else float('nan'):.3f}s  {'OK' if ok else 'MISMATCH'}", flush=True)
        if not ok:
            sys.exit("ghost replay mismatch: decode/engine disagree, search results would be meaningless")

    best = None  # (time_ms, inputs, kinds)
    finishes = []  # every finish seen: (time_ms, origin of the last segment, ghost lineage or None)
    trail = []     # per step: completion of the ghost-0 line minus completion of the best non-ghost node
    t0 = time.time()
    step = 0
    kind_wins = Counter()
    while nodes and step < args.max_steps:
        step += 1
        frame = nodes[0].frame
        seg = args.seg

        # --- expand ------------------------------------------------------------------------------------
        msgs = {}
        for n in nodes:
            specs = []
            if n.lineage is not None:
                gf = ghosts[n.lineage]["frames"]
                exact = list(gf[frame:frame + seg])
                specs.append(("ghost", exact, "ghost"))
                for _ in range(args.mutations):
                    specs.append(("ghost", mutate(exact, rng), "ghost_mut"))
            if not args.no_policy:
                specs.append(("policy", 0.0, 0, "policy"))
                for temp in (0.8, 1.3):
                    specs.append(("policy", temp, int(rng.integers(1 << 30)), "policy_T"))
                for stick in (-1.0, 1.0, 0.0):
                    specs.append(("forced", stick, int(rng.integers(4, 11)), 0.0, 0, "forced"))
            wire = [s[:-1] for s in specs]  # the trailing kind label stays on this side
            msgs.setdefault(n.worker, []).append((n, wire, [s[-1] for s in specs]))
        # Each worker expands its own nodes one after another; different workers run in parallel.
        queues = {w: list(items) for w, items in msgs.items()}
        children = []
        while any(queues.values()):
            batch = {}
            for w, q in queues.items():
                if q:
                    n, wire, labels = q.pop(0)
                    batch[w] = (n, wire, labels)
            replies = pool.call_all({w: ("expand", n.id, n.row, wire, seg) for w, (n, wire, labels) in batch.items()})
            for w, (n, wire, labels) in batch.items():
                for (inputs, row, end), label, spec in zip(replies[w], labels, wire):
                    children.append(dict(parent=n, inputs=inputs, row=row, end=end, kind=label,
                            lineage=n.lineage if label == "ghost" else None))

        # --- finished children are candidate answers, not beam members -----------------------------------
        alive = []
        for c in children:
            if c["end"] is not None:
                t_ms = c["end"]["timeMs"]
                finishes.append((t_ms, c["kind"], c["lineage"], c["parent"].inputs + c["inputs"]))
                if best is None or t_ms < best[0]:
                    best = (t_ms, c["parent"].inputs + c["inputs"], c["parent"].kinds + [c["kind"]])
                    print(f"  step {step} frame {frame + len(c['inputs'])}: FINISH {t_ms / 1000:.3f}s via {c['kind']}", flush=True)
            else:
                c["value"] = node_value(c["row"], cols)
                alive.append(c)

        # --- select ---------------------------------------------------------------------------------------
        alive.sort(key=lambda c: -c["value"])
        chosen, seen, per_parent = [], set(), Counter()
        protected = {}
        for c in alive:
            if c["kind"] == "ghost" and c["lineage"] is not None:
                protected[c["lineage"]] = c  # exact ghost continuations are always kept
        for c in alive:
            if len(chosen) >= args.beam:
                break
            row = c["row"]
            key = (int(row[cols.index("px")] // 10), int(row[cols.index("pz")] // 10), int(row[si] // 2),
                   int(row[cols.index("driftState")]))
            if key in seen or per_parent[id(c["parent"])] >= args.max_per_parent:
                continue
            seen.add(key)
            per_parent[id(c["parent"])] += 1
            chosen.append(c)
        for c in protected.values():
            if not any(c is x for x in chosen):
                chosen.append(c)

        # --- materialize the survivors in their parents' workers ------------------------------------------
        by_worker = {}
        for c in chosen:
            by_worker.setdefault(c["parent"].worker, []).append(c)
        new_nodes = []
        queues = {w: list(cs) for w, cs in by_worker.items()}
        while any(queues.values()):
            batch = {w: q.pop(0) for w, q in queues.items() if q}
            nids = {w: new_id() for w in batch}
            rows = pool.call_all({w: ("materialize", c["parent"].id, c["inputs"], nids[w]) for w, c in batch.items()})
            for w, c in batch.items():
                p = c["parent"]
                n = Node(nids[w], w, p.frame + len(c["inputs"]), rows[w], p.inputs + c["inputs"], c["lineage"], p.kinds + [c["kind"]])
                n.value = c["value"]
                new_nodes.append(n)
        pool.call_all({w: ("free", [n.id for n in nodes if n.worker == w]) for w in {n.worker for n in nodes}})
        free_ids.extend(n.id for n in nodes)
        nodes = new_nodes

        if not nodes:
            break
        top = max(nodes, key=lambda n: n.value)
        kind_wins[top.kinds[-1]] += 1
        g0 = [n for n in nodes if n.lineage == 0]
        off = [n for n in nodes if n.lineage is None]
        if g0 and off:
            trail.append(g0[0].row[ci] - max(n.row[ci] for n in off))
        if step % 10 == 0 or step == 1:
            lead = ", ".join(f"{k}:{v}" for k, v in Counter(n.kinds[-1] for n in nodes).items())
            mi = cols.index("mushrooms")
            g0n = [n for n in nodes if n.lineage == 0]
            gm = f" ghost0 mush {int(g0n[0].row[mi])}" if g0n else ""
            print(f"step {step:3d} frame {top.frame}  best completion {top.row[ci]:.4f} speed {top.row[si]:.1f} "
                  f"mush {int(top.row[mi])}{gm}  "
                  f"beam {len(nodes)} [{lead}]  {time.time() - t0:.0f}s", flush=True)

        # A node can't beat the best finish once it is past that many frames.
        if best is not None and nodes and nodes[0].frame > GO_FRAME + 0 and nodes[0].frame >= len(best[1]):
            break

    pool.close()

    if trail:
        tr = np.array(trail)
        print(f"\nghost-0 line minus best non-ghost node, in completion: mean {tr.mean():+.4f}, "
              f"min {tr.min():+.4f}, max {tr.max():+.4f}; non-ghost node ahead in {(tr < 0).sum()}/{len(tr)} steps")
    if finishes:
        finishes.sort(key=lambda f: f[0])
        print("fastest finishes (ms, last segment origin, protected-ghost id):")
        for f in finishes[:8]:
            print(f"   {f[0]}  {f[1]}  ghost={f[2]}")
        print(f"   ... {len(finishes)} finishes total; fastest by non-ghost-origin last segment: "
              + str(next((f[0] for f in finishes if f[1] not in ('ghost',)), None)))

    if best is None:
        print("no run finished")
        return 1

    t_ms, inputs, kinds = best
    print(f"\nbest finish: {t_ms / 1000:.3f}s   ghosts: " + ", ".join(f"{g['time_ms'] / 1000:.3f}s" for g in ghosts)
          + f"   ({time.time() - t0:.0f}s)")
    print("segments of the winning run by origin: " + ", ".join(f"{k}:{v}" for k, v in Counter(kinds).items()))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, "wb").write(encode_rkg(args.course, args.character, args.vehicle, inputs, t_ms))
    print(f"wrote {args.out}")

    if args.save_top:
        os.makedirs(args.top_dir, exist_ok=True)
        seen_runs, written = set(), 0
        for t_ms_f, kind_f, _lin, inp_f in finishes:
            key = hash(tuple(inp_f))
            if key in seen_runs:
                continue
            seen_runs.add(key)
            name = f"run-{t_ms_f:06d}-{written:02d}.rkg"
            open(os.path.join(args.top_dir, name), "wb").write(
                    encode_rkg(args.course, args.character, args.vehicle, inp_f, t_ms_f))
            written += 1
            if written >= args.save_top:
                break
        print(f"saved {written} distinct finished runs to {args.top_dir}")

    # Round-trip check: decode the file we just wrote and run it in a fresh engine.
    frames = decode_rkg(args.out)[3]
    with KinokoSearch(args.binary, args.course, args.character, args.vehicle) as race:
        for f in frames:
            race.step(f[0], f[2], f[3], f[1])
            if race.end:
                break
        end = race.end
    ok = end is not None and end["timeMs"] == t_ms
    print(f"round-trip of the written .rkg: {'MATCH' if ok else 'MISMATCH'} "
          f"({end['timeMs'] / 1000 if end else float('nan'):.3f}s)")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
