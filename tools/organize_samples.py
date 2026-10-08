#!/usr/bin/env python3
"""One-off reorganisation of samples/ into per-track folders (2026-10-02), plus the path fixes that go with it.

    samples/tracks/<track>/ghosts/   reference and downloaded ghosts for that track
    samples/tracks/<track>/runs/     search outputs (beam, brute force, GPU runs, ...)
    samples/data/                    non-ghost data: policy dumps, DAgger checkpoints, evaluation logs
    samples/*.krkg, samples/*.rkg    the Kinoko repo's own test samples (git-tracked, testCases.json) -- not moved

Moves only files that are NOT tracked by git, then rewrites every reference to a moved path in tools/, docs/ and the
Claude memory folder (both "samples/x" strings and os.path.join(..., "samples", "x") calls).

    python tools/organize_samples.py            # dry run: prints every move and every file it would edit
    python tools/organize_samples.py --apply
"""

import argparse
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = os.path.join(ROOT, "samples")
MEMORY = os.path.expanduser(r"~\.claude\projects\C--Users-dwain-Documents-New-2-DolphinLuaServer-main\memory")

# course id -> folder name (the GUI's track list, slugged)
TRACKS = {
    8: "luigi-circuit", 1: "moo-moo-meadows", 2: "mushroom-gorge", 4: "toads-factory", 0: "mario-circuit",
    5: "coconut-mall", 6: "dk-summit", 7: "warios-gold-mine", 9: "daisy-circuit", 15: "koopa-cape",
    11: "maple-treeway", 3: "grumble-volcano", 14: "dry-dry-ruins", 10: "moonview-highway", 12: "bowsers-castle",
    13: "rainbow-road", 16: "gcn-peach-beach", 20: "ds-yoshi-falls", 25: "snes-ghost-valley-2",
    26: "n64-mario-raceway", 27: "n64-sherbet-land", 31: "gba-shy-guy-beach", 23: "ds-delfino-square",
    18: "gcn-waluigi-stadium", 21: "ds-desert-hills", 30: "gba-bowser-castle-3", 29: "n64-dks-jungle-parkway",
    17: "gcn-mario-circuit", 24: "snes-mario-circuit-3", 22: "ds-peach-gardens", 19: "gcn-dk-mountain",
    28: "n64-bowsers-castle",
}
LC = "tracks/luigi-circuit"
YF = "tracks/ds-yoshi-falls"

# old (relative to samples/) -> new (relative to samples/)
MOVES = {
    "LC_3lap_67.788_ref.rkg": f"{LC}/ghosts/LC_3lap_67.788_ref.rkg",
    "LC_41.105s_41b3f3e.rkg": f"{LC}/ghosts/LC_41.105s_41b3f3e.rkg",
    "lc-rta-0-0-0.rkg": f"{LC}/ghosts/lc-rta-0-0-0.rkg",
    "tas": f"{LC}/ghosts/tas",
    "lc-top100-pb": f"{LC}/ghosts/lc-top100-pb",
    "lc-top20": f"{LC}/ghosts/lc-top20",
    "lc-top20-unique": f"{LC}/ghosts/lc-top20-unique",
    "lc-diverse": f"{LC}/ghosts/lc-diverse",
    "best": f"{LC}/runs/best",
    "beam": f"{LC}/runs/beam",
    "evolve": f"{LC}/runs/evolve",
    "gpu_beam": f"{LC}/runs/gpu_beam",
    "gpu_brute": f"{LC}/runs/gpu_brute",
    "gpu_chain": f"{LC}/runs/gpu_chain",
    "gpu_countdown": f"{LC}/runs/gpu_countdown",
    "rYF_57.768s_ac4e72f0.rkg": f"{YF}/ghosts/rYF_57.768s_ac4e72f0.rkg",
    "rYF brute_best_701.4.rkg": f"{YF}/runs/brute/rYF brute_best_701.4.rkg",
    "dumps": "data/dumps",
    "dagger": "data/dagger",
    "eval_log.jsonl": "data/eval_log.jsonl",
    "quick_log.jsonl": "data/quick_log.jsonl",
}
for f in sorted(glob.glob(os.path.join(S, "LC brute_best_*.rkg")) + glob.glob(os.path.join(S, "LC seg*.rkg"))):
    MOVES[os.path.basename(f)] = f"{LC}/runs/brute/{os.path.basename(f)}"

README = """# samples/

    tracks/<track>/ghosts/   reference and downloaded ghosts for that track
    tracks/<track>/runs/     search outputs (beam search, brute force, GPU runs, ...)
    data/                    non-ghost data: policy dumps, DAgger checkpoints, evaluation logs
    *.krkg, *.rkg (here)     the Kinoko repo's own test samples, used by testCases.json -- leave these here

Track folders and the course id the tools take (--course):

""" + "\n".join(f"    {cid:2d}  {name}" for cid, name in sorted(TRACKS.items())) + "\n"

TEXT_GLOBS = ["tools/**/*.py", "tools/**/*.sh", "tools/**/*.html", "docs/*.md", "samples/**/*.sh", "*.md"]


def tracked():
    out = subprocess.run(["git", "ls-files", "samples"], cwd=ROOT, capture_output=True, text=True).stdout
    return {line[len("samples/"):] for line in out.splitlines()}


def rewrite(text):
    """Every reference to a moved path -> its new path. Longest names first (lc-top20-unique before lc-top20)."""
    n = 0
    for old in sorted(MOVES, key=len, reverse=True):
        new = MOVES[old]
        # "samples/old" or "samples\\old", not followed by more of a name
        pat = re.compile(r"(samples[/\\]+)" + re.escape(old) + r"(?![\w-])")
        text, k = pat.subn(lambda m: m.group(1) + new.replace("/", "\\" if "\\" in m.group(1) else "/"), text)
        n += k
        # os.path.join(..., "samples", "old", ...)
        parts = ", ".join(f'"{p}"' for p in new.split("/"))
        pat = re.compile(r'"samples",\s*"' + re.escape(old) + '"')
        text, k = pat.subn('"samples", ' + parts, text)
        n += k
    return text, n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()
    git = tracked()
    for old in MOVES:
        if old in git or any(t.startswith(old + "/") for t in git):
            sys.exit(f"refusing: {old} is tracked by git")

    print("moves:")
    for old, new in MOVES.items():
        src, dst = os.path.join(S, old), os.path.join(S, new)
        if not os.path.exists(src):
            print(f"   (missing, skipped) {old}")
            continue
        if os.path.exists(dst):
            sys.exit(f"refusing: destination exists: {new}")
        print(f"   {old}  ->  {new}")
        if a.apply:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.rename(src, dst)

    files = set()
    for g in TEXT_GLOBS:
        files.update(glob.glob(os.path.join(ROOT, g), recursive=True))
    files.update(glob.glob(os.path.join(MEMORY, "*.md")))
    files = sorted(f for f in files if ".bak" not in os.path.basename(f) and os.sep + "xsrc" + os.sep not in f
                   and not f.endswith("organize_samples.py"))
    print("\npath references:")
    total = 0
    for f in files:
        try:
            text = open(f, encoding="utf-8", newline="").read()  # keep CRLF/LF as is
        except (UnicodeDecodeError, OSError):
            continue
        new, n = rewrite(text)
        if n:
            total += n
            print(f"   {n:3d}  {os.path.relpath(f, ROOT) if f.startswith(ROOT) else f}")
            if a.apply:
                with open(f, "w", encoding="utf-8", newline="") as fh:
                    fh.write(new)
    print(f"   {total} references in total")

    if a.apply:
        for name in TRACKS.values():
            for sub in ("ghosts", "runs"):
                os.makedirs(os.path.join(S, "tracks", name, sub), exist_ok=True)
        with open(os.path.join(S, "README.md"), "w", encoding="utf-8") as fh:
            fh.write(README)
        print(f"\ncreated {len(TRACKS)} track folders (ghosts/ + runs/) and samples/README.md")
    else:
        print("\ndry run -- nothing changed. Re-run with --apply.")


if __name__ == "__main__":
    main()
