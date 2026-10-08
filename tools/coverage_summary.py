"""Summarise gcov JSON output: how many executable source lines each part of Kinoko ran in the replays.

    cd <dir with *.gcov.json.gz from `gcov -i`>; python <this file>
"""
import collections
import glob
import gzip
import json

exec_lines = collections.defaultdict(set)
all_lines = collections.defaultdict(set)
for p in glob.glob("*.gcov.json.gz"):
    d = json.load(gzip.open(p))
    for f in d["files"]:
        name = f["file"].replace(chr(92), "/")
        if "/source/" not in name and not name.startswith("source/"):
            continue
        name = name.split("source/", 1)[1]
        for line in f["lines"]:
            all_lines[name].add(line["line_number"])
            if line["count"] > 0:
                exec_lines[name].add(line["line_number"])

groups = collections.defaultdict(lambda: [0, 0, 0])
for n in all_lines:
    parts = n.split("/")
    g = parts[0] + ("/" + parts[1] if parts[0] == "game" and len(parts) > 2 else "")
    groups[g][0] += len(all_lines[n])
    groups[g][1] += len(exec_lines[n])
    groups[g][2] += 1

tot_all = sum(v[0] for v in groups.values())
tot_ex = sum(v[1] for v in groups.values())
print(f"{'area':22s} {'executable lines':>17s} {'executed':>10s}")
for g, (a, e, n) in sorted(groups.items(), key=lambda kv: -kv[1][1])[:12]:
    print(f"{g:22s} {a:17d} {e:10d}   ({100 * e / max(a, 1):.0f}%)")
print(f"{'TOTAL':22s} {tot_all:17d} {tot_ex:10d}   ({100 * tot_ex / tot_all:.0f}%)")
print()
for c, n in sorted(((len(exec_lines[n]), n) for n in exec_lines), reverse=True)[:14]:
    print(f"  {c:5d} executed lines  {n}")
