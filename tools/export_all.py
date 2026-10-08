"""Runs every asset exporter the web player needs, in order, against your own Mario Kart Wii files.

Usage:
    python tools/export_all.py <game folder> [--only step ...] [--skip step ...] [--list]

<game folder> is the extracted contents of your game disc's data partition (the folder that contains Race/, Scene/ and sound/).
Everything is written to web/assets, which is git-ignored: it is derived from Nintendo's files and must not be redistributed or
committed. To use the page without hosting the assets yourself, open it and pick that web/assets folder (web/assetgate.js).

A failing step does not stop the others; the summary at the end lists what failed so it can be re-run with --only.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, '..', 'web', 'assets'))
sys.path.insert(0, HERE)


def py(script, *args):
    return [sys.executable, os.path.join(HERE, script), *args]


def copy_common(mk):
    """The page loads the game's shared archive (physics parameters, object flow table) as assets/common/Common.szs, and the asset
    picker uses it to recognise a valid assets folder. No exporter makes it: it is a plain copy of Race/Common.szs."""
    def run():
        import shutil
        out = os.path.join(ASSETS, 'common')
        os.makedirs(out, exist_ok=True)
        shutil.copyfile(os.path.join(mk, 'Race', 'Common.szs'), os.path.join(out, 'Common.szs'))
        print('copied Race/Common.szs -> %s' % os.path.join(out, 'Common.szs'))
        return 0
    run.label = 'copy Common.szs'
    return run


def ui_language(mk):
    """The race HUD art comes in per-language archives (Scene/UI/Race_<letter>.szs); use English if present, else any."""
    ui = os.path.join(mk, 'Scene', 'UI')
    found = sorted(f[len('Race_'):-len('.szs')] for f in os.listdir(ui) if f.startswith('Race_') and f.endswith('.szs')) if os.path.isdir(ui) else []
    for want in ('U', 'E'):
        if want in found:
            return want
    return found[0] if found else None


def build_steps(mk):
    import export_tracks
    steps = [
        ('common', 'copy Race/Common.szs (the page and the asset picker need it)', [copy_common(mk)]),
        ('tracks', 'track + sky models, course data, objects (every course)', [py('export_tracks.py', mk)]),
        ('enemy', 'CPU routes', [py('export_enemy_paths.py')]),
        ('minimap', 'minimaps', [py('export_minimap.py')]),
        ('bloom', 'bloom settings', [py('export_bloom.py', mk)]),
        ('dof', 'depth-of-field settings', [py('export_dof.py', mk)]),
        ('effects', 'particle effects (shared + each course\'s own)',
         [py('export_effects.py', mk)] + [py('export_effects.py', '--course', t, mk) for t in export_tracks.RACE_TRACKS]),
    ]
    lang = ui_language(mk)
    if lang:
        steps.append(('hud', 'race HUD art (language %s)' % lang, [py('export_hud.py', os.path.join(mk, 'Scene', 'UI'), lang)]))
    steps += [
        ('sounds', 'sound effects + course music', [py('export_web_sounds.py', mk)]),
        ('karts', 'kart + driver models (every vehicle/character)', [py('brres_to_glb.py', mk)]),
        ('ghosts', 'sample ghosts (samples/*.rkg; only with --only ghosts)', [py('prepare_ghosts.py')]),
    ]
    return steps


def check_game_folder(mk):
    """Returns a list of problems; the first few are fatal, the rest only cost a step."""
    need = [('Race/Common.szs', True), ('Race/Course', True), ('Race/Kart', True),
            ('Scene/UI', False), ('sound/revo_kart.brsar', False), ('sound/strm', False)]
    low = {n.lower(): n for n in os.listdir(mk)} if os.path.isdir(mk) else {}
    bad = []
    if not low:
        return [('%s is not a folder' % mk, True)]
    for rel, fatal in need:
        if not os.path.exists(os.path.join(mk, *rel.split('/'))):
            bad.append(('missing %s' % rel, fatal))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mkwii', nargs='?', help='your extracted game files (contains Race/, Scene/, sound/)')
    ap.add_argument('--only', nargs='*', metavar='STEP', help='run just these steps')
    ap.add_argument('--skip', nargs='*', default=[], metavar='STEP', help='skip these steps')
    ap.add_argument('--list', action='store_true', help='list the steps and exit')
    args = ap.parse_args()

    if args.list:
        for name, what, cmds in build_steps(args.mkwii or '.'):
            print('%-8s %s' % (name, what))
        return 0
    if not args.mkwii:
        ap.error('give the path to your extracted game files')
    mk = os.path.abspath(args.mkwii)

    problems = check_game_folder(mk)
    for msg, fatal in problems:
        print(('ERROR: ' if fatal else 'warning: ') + msg)
    if any(f for _, f in problems):
        print('That does not look like the extracted game data (expected Race/Common.szs, Race/Course, Race/Kart).')
        return 2

    steps = build_steps(mk)
    names = [n for n, _, _ in steps]
    for n in (args.only or []) + args.skip:
        if n not in names:
            ap.error('unknown step %r (steps: %s)' % (n, ', '.join(names)))
    # 'ghosts' rewrites web/assets/ghosts/ghosts.json, so it only runs when asked for by name.
    todo = [s for s in steps if (s[0] in args.only if args.only else s[0] != 'ghosts') and s[0] not in args.skip]

    os.makedirs(ASSETS, exist_ok=True)
    results = []
    t_all = time.time()
    for i, (name, what, cmds) in enumerate(todo):
        print('\n[%d/%d] %s: %s' % (i + 1, len(todo), name, what), flush=True)
        t0, failed = time.time(), []
        for j, cmd in enumerate(cmds):
            if len(cmds) > 1 and (j % 8 == 0 or j == len(cmds) - 1):
                print('   (%d/%d)' % (j + 1, len(cmds)), flush=True)
            try:
                bad = cmd() != 0 if callable(cmd) else subprocess.call(cmd, cwd=HERE) != 0
            except Exception as e:
                print('  ! %s' % e)
                bad = True
            if bad:
                failed.append(cmd.label if callable(cmd) else ' '.join(os.path.basename(c) for c in cmd[1:3]))
        results.append((name, time.time() - t0, failed))

    print('\n==== summary (%.0f s total) ====' % (time.time() - t_all))
    for name, secs, failed in results:
        print('%-8s %6.0f s  %s' % (name, secs, 'FAILED: ' + '; '.join(failed) if failed else 'ok'))
    bad = [r for r in results if r[2]]
    if bad:
        print('\nRe-run just the failed steps with:  python tools/export_all.py "%s" --only %s' % (mk, ' '.join(r[0] for r in bad)))
    print('\nOutput: %s' % ASSETS)
    print('Open the page and choose that folder when asked (see web/assetgate.js).')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
