"""Prepare race tracks for the web player from your own Mario Kart Wii files.

Usage:
    python tools/export_tracks.py [path/to/MKWii] [--only folder_name ...]

For each of the 32 race courses, in web/assets/tracks/<folder>/:
    course.szs   copy of Race/Course/<folder>.szs (Kinoko's physics loads it inside the browser)
    objects.json + obj/*.glb   the objects placed on the course (see export_objects.py)
    track.glb    the visible track from course_model.brres, in game units
    sky.glb      the skybox from vrcorn_model.brres (drawn around the camera)

These are Nintendo's assets: they are git-ignored and must not be redistributed.
"""

import argparse
import filecmp
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import brres_to_glb as b  # noqa: E402
import export_objects  # noqa: E402

TRACKS_OUT = os.path.join(HERE, '..', 'web', 'assets', 'tracks')

# Kinoko course IDs 0-31 (include/Common.hh COURSE_NAMES): the race tracks in the menu
RACE_TRACKS = b.COURSE_NAMES[:32] if hasattr(b, 'COURSE_NAMES') else [
    'castle_course', 'farm_course', 'kinoko_course', 'volcano_course', 'factory_course',
    'shopping_course', 'boardcross_course', 'truck_course', 'beginner_course', 'senior_course',
    'ridgehighway_course', 'treehouse_course', 'koopa_course', 'rainbow_course', 'desert_course',
    'water_course', 'old_peach_gc', 'old_mario_gc', 'old_waluigi_gc', 'old_donkey_gc',
    'old_falls_ds', 'old_desert_ds', 'old_garden_ds', 'old_town_ds', 'old_mario_sfc',
    'old_obake_sfc', 'old_mario_64', 'old_sherbet_64', 'old_koopa_64', 'old_donkey_64',
    'old_koopa_gba', 'old_heyho_gba',
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mkwii', nargs='?', default=r'C:\Users\dwain\MKWii')
    ap.add_argument('--only', nargs='*', help='only these course folders')
    args = ap.parse_args()
    export_objects.MKWII = args.mkwii   # object models shared by every course come from this game folder's Race/Common.szs

    course_dir = os.path.join(args.mkwii, 'Race', 'Course')
    flow = export_objects.read_obj_flow(b.load_common(args.mkwii))
    todo = [t for t in RACE_TRACKS if not args.only or t in args.only]
    problems = []
    for n, folder in enumerate(todo):
        src = os.path.join(course_dir, folder + '.szs')
        out = os.path.join(TRACKS_OUT, folder)
        if not os.path.exists(src):
            problems.append((folder, 'missing ' + src))
            continue
        os.makedirs(out, exist_ok=True)
        dst = os.path.join(out, 'course.szs')
        if os.path.exists(dst) and not filecmp.cmp(src, dst, shallow=False):
            print('  note: %s/course.szs differs from the game file; replacing it' % folder)
        if not os.path.exists(dst) or not filecmp.cmp(src, dst, shallow=False):
            shutil.copyfile(src, dst)

        t = time.time()
        try:
            files = b.u8_files(b.yaz0_decompress(open(src, 'rb').read()))
            stats = b.convert_course(files, out)
            objs = export_objects.export_course_objects(files, out, flow, os.path.join(course_dir, 'Object'))
        except Exception as e:
            problems.append((folder, repr(e)))
            continue
        track = stats.get('track.glb', {})
        print('%2d/%d %-20s %6d tris  %2d textures  %5.1f MB  sky:%s  %3d objects (%2d models)  %.0fs' % (
            n + 1, len(todo), folder, track.get('triangles', 0), track.get('textures', 0),
            track.get('bytes', 0) / 1e6, 'yes' if 'sky.glb' in stats else 'no', objs['placed'], objs['models'],
            time.time() - t), flush=True)
        for err in stats.get('errors', []):
            problems.append((folder, err))

    print('\nDone: %d tracks in %s' % (len(todo) - len({p[0] for p in problems}), os.path.abspath(TRACKS_OUT)))
    for folder, msg in problems:
        print('  ! %s: %s' % (folder, msg))


if __name__ == '__main__':
    main()
