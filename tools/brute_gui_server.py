#!/usr/bin/env python3
"""Local GUI for tools/brute_force.py.

A dependency-free control panel (stdlib http.server only, matching the rest of tools/*.py) so you
don't have to drive the search from the command line: pick course/character/vehicle/iteration
count in the browser, start/stop a run, watch the best time found so far update live, and download
the winning attempt as a .rkg once you're happy with it.

    python tools/brute_gui_server.py --binary build-host/kinoko_host

Then open http://localhost:8790 . This is a v1 scaffold: one search running at a time, in-memory
only (nothing persists across a server restart), meant to grow alongside brute_force.py rather
than to be a finished product.
"""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

import brute_force as bf

STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "brute_gui")

_lock = threading.Lock()
_search: "bf.BruteForceSearch | None" = None
_default_binary = None


def _current_status() -> dict:
    with _lock:
        if _search is None:
            return {"running": False, "iteration": 0, "totalIterations": 0, "bestFitness": None,
                    "bestFinished": False, "bestTimeMs": None, "bestCompletion": 0,
                    "bestSpeed": None, "bestProgress": None, "bestBoostCharge": None,
                    "bestBoostFrames": None, "iterationsSinceImprovement": 0, "stagnant": False,
                    "msPerIteration": None,
                    "etaSeconds": None, "error": None, "hasResult": False, "engine": None}
        status = _search.status()
        genome, _result = _search.best()
        status["hasResult"] = genome is not None
        status.setdefault("engine", "cpu")
        return status


def _release_gpu():
    """Free the previous GPU search's race slots before a new one allocates its own (they are ~1.2 GB at the
    default batch, on an 8 GB card). Called with _lock held and no search running."""
    global _search
    if _search is not None and getattr(_search, "gpu", None) is not None:
        _search.gpu = None
        import cupy
        cupy.get_default_memory_pool().free_all_blocks()


def _start_search(body: dict) -> dict:
    global _search

    with _lock:
        if _search is not None and _search.status()["running"]:
            raise RuntimeError("a search is already running -- stop it first")

        n_points = int(body.get("n_points", 24))
        step = int(body.get("step", 15))
        search_start_boost = bool(body.get("search_start_boost", False))
        search_accelerate = bool(body.get("search_accelerate", False))
        search_trick = bool(body.get("search_trick", False))

        prefix_rkg = body.get("prefix_rkg") or None
        prefix_frame_count = (int(body["prefix_frame_count"])
                if body.get("prefix_frame_count") not in (None, "") else None)

        seed_genome = None
        seed_rkg = body.get("seed_rkg")
        if seed_rkg:
            seed_genome = bf.genome_from_rkg(seed_rkg, step, n_points, search_start_boost,
                    search_accelerate, search_trick, start_frame=prefix_frame_count or 0)

        cfg = bf.SearchConfig(
                binary=body.get("binary") or _default_binary,
                course=int(body["course"]),
                character=int(body["character"]),
                vehicle=int(body["vehicle"]),
                iterations=int(body.get("iterations", 500)),
                n_points=n_points,
                step=step,
                max_frames=int(body.get("max_frames", 10800)),
                mutation_rate=float(body.get("mutation_rate", 0.3)),
                mutation_sigma=float(body.get("mutation_sigma", 0.15)),
                seed_genome=seed_genome,
                fitness_metric=body.get("fitness_metric", "completion"),
                search_start_boost=search_start_boost,
                search_accelerate=search_accelerate,
                search_trick=search_trick,
                reference_rkg=body.get("reference_rkg") or None,
                progress_start_frame=int(body.get("progress_start_frame", bf.COUNTDOWN_START_FRAME)),
                parallel_workers=int(body.get("parallel_workers", 1)),
                boost_bonus_weight=float(body.get("boost_bonus_weight", 0.0)),
                min_boost_charge=(float(body["min_boost_charge"])
                        if body.get("min_boost_charge") not in (None, "") else None),
                speed_bonus_weight=float(body.get("speed_bonus_weight", 0.0)),
                max_boost_charge=(float(body["max_boost_charge"])
                        if body.get("max_boost_charge") not in (None, "") else None),
                accel_hold_bonus_weight=float(body.get("accel_hold_bonus_weight", 0.0)),
                stagnation_stop_after=(int(body["stagnation_stop_after"])
                        if body.get("stagnation_stop_after") else None),
                completion_bonus_weight=float(body.get("completion_bonus_weight", 0.0)),
                z_bonus_weight=float(body.get("z_bonus_weight", 0.0)),
                completion_tolerance=int(body.get("completion_tolerance", 1)),
                eval_log_path=body.get("eval_log_path") or None,
                surrogate_path=body.get("surrogate_path") or None,
                surrogate_oversample=int(body.get("surrogate_oversample", 4)),
                prefix_rkg=prefix_rkg,
                prefix_frame_count=prefix_frame_count,
                ternary_steering=bool(body.get("ternary_steering", False)),
                heading_checkpoint_frame=(int(body["heading_checkpoint_frame"])
                        if body.get("heading_checkpoint_frame") not in (None, "") else None),
                heading_bonus_weight=float(body.get("heading_bonus_weight", 0.0)),
        )
        engine = body.get("engine", "cpu")
        if engine == "gpu":
            # GPU engine (tools/gpu_brute.py): same SearchConfig and fitness, thousands of candidates per
            # generation. cfg.binary is only used for its CPU cross-checks, so it defaults to the
            # build-gpu-spike binary (the CPU build with the GPU's heap layout). Imported here so the CPU-only
            # GUI never needs cupy.
            import gpu_brute
            if not body.get("binary"):
                cfg.binary = gpu_brute.CPU_BINARY
            _release_gpu()
            window = None
            if body.get("edit_window_from") not in (None, "") or body.get("edit_window_to") not in (None, ""):
                window = (int(body.get("edit_window_from") or 0), int(body.get("edit_window_to") or cfg.max_frames))
            _search = gpu_brute.make_search(cfg,
                    edit_base_rkg=body.get("edit_base_rkg") or None,
                    edit_base_shifts=[int(s) for s in str(body.get("edit_base_shifts") or "0").split(",")],
                    edits_per_child=float(body.get("edits_per_child") or 2.0),
                    edit_window=window,
                    batch=int(body.get("gpu_batch") or 2048),
                    parents=int(body.get("gpu_parents") or 1),
                    seed=int(body["gpu_seed"]) if body.get("gpu_seed") not in (None, "") else None,
                    verify_every=int(body.get("gpu_verify_every") or 0))
            _search.start()
            return {"started": True, "engine": "gpu"}

        if not cfg.binary:
            raise RuntimeError("no kinoko_host binary configured (pass --binary to the server, "
                    "or 'binary' in the start request)")

        _search = bf.BruteForceSearch(cfg)
        _search.start()
        return {"started": True}


def _stop_search() -> dict:
    with _lock:
        if _search is not None:
            _search.stop()
        return {"stopped": True}


def _download_rkg() -> tuple[bytes, str]:
    with _lock:
        if _search is None:
            raise RuntimeError("no search has run yet")
        # best_snapshot() reads genome/result/progress under one lock acquisition inside
        # BruteForceSearch, so they can never be torn across a background iteration landing
        # mid-read the way separate best() + status() calls could (a stale-filename bug this
        # session hit: a downloaded file was labeled with a newer iteration's score than the one
        # actually encoded into it).
        cfg = _search.config
        status = _search.status()
        if hasattr(_search, "best_frames_snapshot"):
            # GPU search: hands out the frames directly (in edit mode there is no genome to rebuild them from)
            frames, result, progress = _search.best_frames_snapshot()
            if frames is None:
                raise RuntimeError("no result yet")
        else:
            genome, result, progress = _search.best_snapshot()
            locked_prefix = _search.locked_prefix_frames()
            if genome is None:
                raise RuntimeError("no result yet")
            frames = None

    if frames is None:
        frames = list(bf.frames_from_genome(genome, cfg.step, cfg.max_frames, cfg.search_start_boost,
                cfg.search_accelerate, cfg.search_trick, cfg.n_points, locked_prefix,
                cfg.ternary_steering))
    rkg = bf.encode_rkg(cfg.course, cfg.character, cfg.vehicle, frames,
            result["timeMs"] if result["finished"] else 0)

    if progress is not None:
        label = "%.1f" % progress
    elif status.get("bestTimeMs") is not None:
        label = "%dms" % status["bestTimeMs"]
    else:
        label = "unfinished"
    filename = "brute_best_%s.rkg" % label
    return rkg, filename


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # quieter default logging
        pass

    def _send_json(self, obj, status=200):
        payload = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _send_error_json(self, message, status=400):
        self._send_json({"error": message}, status)

    def _serve_static(self, path):
        if path == "/":
            path = "/index.html"
        full = os.path.normpath(os.path.join(STATIC_DIR, path.lstrip("/")))
        if not full.startswith(STATIC_DIR) or not os.path.isfile(full):
            self.send_error(404)
            return

        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        with open(full, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/status":
            self._send_json(_current_status())
        elif path == "/api/download":
            try:
                rkg, filename = _download_rkg()
            except Exception as exc:  # noqa: BLE001
                self._send_error_json(str(exc), 400)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition",
                    "attachment; filename=\"%s\"" % filename)
            self.send_header("Content-Length", str(len(rkg)))
            self.end_headers()
            self.wfile.write(rkg)
        else:
            self._serve_static(path)

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            self._send_error_json("invalid JSON body", 400)
            return

        try:
            if path == "/api/start":
                self._send_json(_start_search(body))
            elif path == "/api/stop":
                self._send_json(_stop_search())
            else:
                self.send_error(404)
        except Exception as exc:  # noqa: BLE001 -- surface config/engine errors to the GUI
            self._send_error_json(str(exc), 400)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", help="default path to the kinoko_host executable")
    parser.add_argument("--port", type=int, default=8790)
    args = parser.parse_args()

    global _default_binary
    _default_binary = args.binary

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Brute-force GUI at http://localhost:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
