# Kinoko web player: getting your game files in

The hosted page contains the physics engine and the viewer only. It has none of Mario Kart Wii's assets: you make them
yourself from your own copy of the game, on your own computer, and the page reads them locally (nothing is uploaded).

## 1. What you need
- Your own Mario Kart Wii disc image (tested with a **PAL** dump; other regions are untested).
- Python 3.10 or newer, plus two add-on packages, `numpy` and `pillow` (see below).
- This repository (`git clone https://github.com/DwainiumB/kinoko-wasm` or "Download ZIP").
- A Chromium browser (Chrome or Edge) is recommended: it can remember the folder you pick. Firefox and Safari work but ask again every visit.

### Installing Python, numpy and pillow
1. **Python:** download it from https://www.python.org/downloads/ and run the installer. On Windows, tick **"Add python.exe to PATH"**
   on the first screen. (On macOS or Linux Python 3 is often already installed.)
2. **Check it works:** open a terminal (Windows: Start menu > "Command Prompt" or "PowerShell"; macOS: Terminal) and run:
   ```
   python --version
   ```
   It should print 3.10 or higher. On macOS/Linux the command may be `python3` instead of `python`; use that name for everything below.
3. **Install the two packages:**
   ```
   python -m pip install numpy pillow
   ```
   (`pillow` is the package that provides the `PIL` module the tools import.) If you see "pip is not recognized", use
   `py -m pip install numpy pillow` on Windows, or `python3 -m pip install numpy pillow` on macOS/Linux.
4. **Check it worked:**
   ```
   python -c "import numpy, PIL; print('ok')"
   ```
   It should print `ok`. If you get "No module named ...", the packages went into a different Python than the one you run: use the
   same command (`python`, `py` or `python3`) for the install and for `export_all.py`.
5. **Windows notes:**
   - If typing `python` opens the Microsoft Store instead of printing a version, Python is not installed yet (or not on PATH): install it
     from python.org as in step 1, then open a *new* terminal window.
   - Put paths that contain spaces in quotes, e.g. `python tools/export_all.py "C:\Games\Mario Kart Wii\files"`.
   - Windows can refuse very long file paths. Extract the game close to the drive root (for example `C:\mkw`) rather than deep inside
     other folders.
6. On Linux, if pip says "externally-managed-environment", make a virtual environment first:
   `python3 -m venv .venv && source .venv/bin/activate`, then run the install again.

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

## If the page says "That folder has no common/Common.szs"
Your `assets` folder was made by an older version of `export_all.py`, which forgot one file. Update the repository and run
`python tools/export_all.py "C:\path\to\extracted\files" --only common` (takes a second), or copy `Race/Common.szs` from your game files into
`web/assets/common/` by hand. Then pick the `assets` folder again.

## Notes
- `web/assets` is derived from Nintendo's files. It is git-ignored; do not upload or share it.
- Running the page from a local server that already has `web/assets` (for example `python -m http.server` inside `web/`) skips the picker.
  Add `?pick=1` to the address to force the picker.
