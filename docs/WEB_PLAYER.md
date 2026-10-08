# Kinoko web player: getting your game files in

The hosted page contains the physics engine and the viewer only. It has none of Mario Kart Wii's assets: you make them
yourself from your own copy of the game, on your own computer, and the page reads them locally (nothing is uploaded).

## 1. What you need
- Your own Mario Kart Wii disc image (tested with a **PAL** dump; other regions are untested).
- Python 3.10+ with `numpy` and `pillow`: `pip install numpy pillow`
- This repository (`git clone https://github.com/DwainiumB/kinoko-wasm` or "Download ZIP").
- A Chromium browser (Chrome or Edge) is recommended: it can remember the folder you pick. Firefox and Safari work but ask again every visit.

## 2. Extract the game files
In Dolphin: right-click the game > Properties > Filesystem tab > right-click **Partition 1: DATA** (the data partition) >
**Extract Entire Partition**, and choose a folder. Use the `files` folder inside the result: it contains `Race/`, `Scene/` and `sound/`.

## 3. Run the exporter
```
python tools/export_all.py "C:\path\to\extracted\files"
```
It takes about 6 minutes and writes `web/assets` (roughly 700 MB). `python tools/export_all.py --list` shows the steps, and
`--only` / `--skip` re-run or skip some. If a step fails, the end of the log prints the command to re-run just that step.

## 4. Open the page and pick the folder
Open https://dwainiumb.github.io/kinoko-wasm/ , press **Choose assets folder**, and select the `web/assets` folder you just
made (the repository folder or `web` also works). Everything is read from your disk by your browser.

## Notes
- `web/assets` is derived from Nintendo's files. It is git-ignored; do not upload or share it.
- Running the page from a local server that already has `web/assets` (for example `python -m http.server` inside `web/`) skips the picker.
  Add `?pick=1` to the address to force the picker.
