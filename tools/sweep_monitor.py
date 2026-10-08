#!/usr/bin/env python3
"""Live progress page for gpu_window_sweep.py logs (read-only: it only parses the log files).

    python tools/sweep_monitor.py --log "tte=samples/tracks/ds-yoshi-falls/runs/lap1_tte/sweep/sweep.log" \\
                                  --log "tte+height+completion=samples/tracks/ds-yoshi-falls/runs/lap1_tte/sweep2/sweep.log"

then open http://localhost:8795 . Each log gets a table with one row per window: its progress through the generations,
the base and current best score, and the gain. For --scoring tte logs the score is an estimated finish time in ms
(lower is better); for the older scoring it is units of progress along the reference line (higher is better).
"""

import argparse
import json
import os
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RE_WIN = re.compile(r"^--- window (\d+)-(\d+), scored @(\d+)")
RE_GEN = re.compile(r"^\s*gen (\d+)/(\d+) fitness=(-?[\d.]+) best=(-?[\d.]+) units of progress .*?\(([\d.]+) ms/gen")
RE_DONE_W = re.compile(r"^window (\d+)-(\d+) scored @(\d+): gain (\S+) (units|ms)")
RE_SKIP = re.compile(r"^window (\d+)-(\d+): SKIPPED")
RE_HEAD = re.compile(r"^(.*): course (\d+), character (\d+), vehicle (\d+); real inputs end at frame (\d+); (\d+) windows")
RE_DONE = re.compile(r"^done in (\d+)s")


def parse(path):
    out = {"path": path, "exists": os.path.exists(path), "windows": [], "head": None, "done": None, "mtime": None}
    if not out["exists"]:
        return out
    out["mtime"] = os.path.getmtime(path)
    cur = None
    seen = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        m = RE_HEAD.match(line)
        if m:
            out["head"] = dict(name=m.group(1), course=int(m.group(2)), end=int(m.group(5)), windows=int(m.group(6)))
            continue
        m = RE_WIN.match(line)
        if m:
            key = (int(m.group(1)), int(m.group(2)))
            cur = seen.get(key)  # a log written by two handles (redirect + the script's own file) repeats windows
            if cur is None:
                cur = dict(a=key[0], b=key[1], score=int(m.group(3)), gens=0, total=0, base=None, best=None,
                           base_units=None, best_units=None, msgen=None, state="running", gain=None)
                seen[key] = cur
                out["windows"].append(cur)
            continue
        m = RE_GEN.match(line)
        if m and cur is not None:
            g = int(m.group(1))
            f, u = float(m.group(3)), float(m.group(4))
            if cur["base"] is None:
                cur["base"], cur["base_units"] = f, u
            if g >= cur["gens"]:
                cur["gens"], cur["total"] = g, int(m.group(2))
                cur["best"], cur["best_units"] = f, u
                cur["msgen"] = float(m.group(5))
            continue
        m = RE_DONE_W.match(line)
        if m:
            w = seen.get((int(m.group(1)), int(m.group(2))))
            if w is not None:
                w["state"] = "done"
                w["gain_text"] = m.group(4) + " " + m.group(5)
            continue
        if RE_SKIP.match(line):
            out["windows"].append(dict(a=int(RE_SKIP.match(line).group(1)), b=int(RE_SKIP.match(line).group(2)),
                                       score=0, gens=0, total=0, base=None, best=None, state="skipped"))
            continue
        m = RE_DONE.match(line)
        if m:
            out["done"] = int(m.group(1))
    for w in out["windows"]:
        if w.get("gain_text"):  # the sweep's own end-of-window summary is authoritative
            num, unit = w["gain_text"].split()
            w["gain"], w["unit"] = float(num), unit
            if w.get("best") is None:
                w["best"] = w["best_units"] = None
            continue
        b, f = w.get("base"), w.get("best")
        if b is None or f is None:
            continue
        if 10_000 < abs(b) < 1_000_000:       # tte: an estimated finish time in ms, lower is better
            w["unit"], w["gain"] = "ms", b - f
        else:                                  # progress: units along the reference line, higher is better
            w["unit"], w["gain"] = "units", w["best_units"] - w["base_units"]
    if out["windows"] and out["done"] is None and out["windows"][-1]["state"] == "running":
        out["windows"][-1]["state"] = "running"
    return out


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sweep monitor</title>
<style>
:root{--bg:#fafaf8;--fg:#1c1c1a;--mut:#6b6b66;--line:#dcdcd6;--card:#fff;--good:#1d7a4d;--bad:#b3402a;--run:#2a63b3;--bar:#e8e8e2}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#161615;--fg:#ececea;--mut:#9a9a94;--line:#33332f;--card:#1e1e1c;--good:#53c08a;--bad:#e8806a;--run:#6b9be0;--bar:#2a2a27}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif}
main{max-width:980px;margin:0 auto;padding:20px 16px 48px}
h1{font-size:20px;margin:0 0 2px}.sub{color:var(--mut);margin:0 0 18px;font-size:13px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin:0 0 16px}
h2{font-size:15px;margin:0 0 2px}.meta{color:var(--mut);font-size:12.5px;margin:0 0 10px}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
th{font-weight:600;font-size:12px;color:var(--mut);text-align:left;padding:4px 8px 6px 0;border-bottom:1px solid var(--line)}
td{padding:7px 8px 7px 0;border-bottom:1px solid var(--line);font-size:14px;vertical-align:middle}
tr:last-child td{border-bottom:0}.num{text-align:right;white-space:nowrap}
.bar{height:8px;background:var(--bar);border-radius:4px;overflow:hidden;min-width:90px}.bar i{display:block;height:100%;background:var(--run)}
.tag{font-size:12px;padding:1px 8px;border-radius:9px;border:1px solid var(--line);color:var(--mut)}
.tag.run{color:var(--run);border-color:var(--run)}.tag.done{color:var(--good);border-color:var(--good)}
.g{display:flex;align-items:center;gap:8px;min-width:170px}.gz{position:relative;flex:1;height:10px;background:var(--bar);border-radius:5px}
.gz:before{content:"";position:absolute;left:50%;top:-2px;width:1px;height:14px;background:var(--mut)}
.gz i{position:absolute;top:0;height:100%;border-radius:5px}
.pos{color:var(--good)}.neg{color:var(--bad)}.mut{color:var(--mut)}
.err{color:var(--bad)}
</style></head><body><main>
<h1>Window sweep monitor</h1>
<p class="sub">Gain = how much a window's best candidate beats the unedited run at the same window. For <b>ms</b> logs it is an
estimated finish time (lower is better, an estimate from the reference ghost, not a measured finish); gains are per window and are
not additive. <span id="upd"></span></p>
<div id="root"></div>
<script>
const $=s=>document.querySelector(s);
const fmt=(v,d=1)=>v==null?'–':(v>0?'+':'')+v.toFixed(d);
function render(data){
  let html='';
  for(const L of data.logs){
    const W=L.windows||[];
    const unit=(W.find(w=>w.unit)||{}).unit||'';
    const maxg=Math.max(1,...W.map(w=>Math.abs(w.gain||0)));
    const status=!L.exists?'<span class="err">log not found</span>':L.done!=null?`finished in ${L.done}s`:'running';
    html+=`<section><h2>${L.name}</h2><p class="meta">${status}${L.head?` · ${L.head.name.split(/[\\/]/).pop()} · edits up to frame ${L.head.end} · ${L.head.windows} windows`:''}${L.path?` · <span class="mut">${L.path}</span>`:''}</p>
    <table><thead><tr><th>Window</th><th>Scored @</th><th>State</th><th>Generations</th><th class="num">Base</th><th class="num">Best</th><th>Gain (${unit||'?'})</th></tr></thead><tbody>`;
    for(const w of W){
      const pct=w.total?Math.round(100*w.gens/w.total):0;
      const st=w.state=='done'?'<span class="tag done">done</span>':w.state=='skipped'?'<span class="tag">skipped</span>':'<span class="tag run">running</span>';
      const g=w.gain, u=w.unit||unit;
      const best=u=='ms'?(w.best==null?'–':w.best.toFixed(1)):(w.best_units==null?'–':w.best_units.toFixed(1));
      const base=u=='ms'?(w.base==null?'–':w.base.toFixed(1)):(w.base_units==null?'–':w.base_units.toFixed(1));
      const wd=g==null?0:Math.min(50,50*Math.abs(g)/maxg);
      const bar=g==null?'':`<i style="${g>=0?'left:50%':`left:${50-wd}%`};width:${wd}%;background:var(--${g>=0?'good':'bad'})"></i>`;
      html+=`<tr><td>${w.a}–${w.b}</td><td>${w.score||'–'}</td><td>${st}</td>
      <td><div class="bar"><i style="width:${pct}%"></i></div><span class="mut" style="font-size:12px">${w.gens}/${w.total||'?'}${w.msgen?` · ${(w.msgen/1000).toFixed(2)} s/gen`:''}</span></td>
      <td class="num">${base}</td><td class="num">${best}</td>
      <td><div class="g"><div class="gz">${bar}</div><b class="${g>0.05?'pos':g<-0.05?'neg':'mut'}" style="min-width:64px;text-align:right">${fmt(g,u=='ms'?1:2)}</b></div></td></tr>`;
    }
    html+='</tbody></table></section>';
  }
  $('#root').innerHTML=html;
  $('#upd').textContent='Updated '+new Date().toLocaleTimeString()+' (refreshes every 2 s)';
}
async function tick(){try{render(await (await fetch('api')).json())}catch(e){$('#upd').textContent='server not reachable'}}
tick();setInterval(tick,2000);
</script></main></body></html>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", action="append", required=True, metavar="NAME=PATH", help="a sweep.log (repeatable)")
    ap.add_argument("--port", type=int, default=8795)
    a = ap.parse_args()
    logs = []
    for item in a.log:
        name, _, path = item.partition("=")
        if not path:
            name, path = os.path.basename(os.path.dirname(item)) or item, item
        logs.append((name, path))

    class H(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if self.path.startswith("/api"):
                body = json.dumps({"logs": [dict(parse(p), name=n) for n, p in logs], "time": time.time()}).encode()
                ctype = "application/json"
            else:
                body, ctype = PAGE.encode(), "text/html; charset=utf-8"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

    print(f"sweep monitor on http://localhost:{a.port}  ({len(logs)} logs)", flush=True)
    ThreadingHTTPServer(("127.0.0.1", a.port), H).serve_forever()


if __name__ == "__main__":
    main()
