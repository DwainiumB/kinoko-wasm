"""Dump every placed object's real KMP data (id, name, position, rotation, scale, settings) for
every race track, in one pass, to a single reusable JSON file.

Why: investigating a single object's real behavior (e.g. "does this object's animation come from
its KMP settings, or is it hardcoded?") kept meaning writing a one-off script, decompressing the
same course archives again, and re-deriving the same GOBJ-parsing logic each time (see the
kinokoT1 investigation, project memory 2026-09-27/28 -- its own settings were never actually
checked until the very end, costing a lot of back-and-forth). This dumps ALL of it once, for every
track, so a future "does object X have real settings anywhere" question is a grep/json-load instead
of a new extraction pass.

Usage:
    python tools/dump_all_object_settings.py [path/to/MKWii]

Output: tools/data/all_object_placements.json --
    {folder: [{id, name, resources, pos, rot, scale, settings, presence}, ...]}
one entry per placed GOBJ with presence bit 0 set (shown in a one-player race), across every
race-track course.kmp (the 32 folders web/assets/tracks already uses -- battle stages and demo
courses aren't included, since the web player doesn't use them; add their folder names to TRACKS
below if that ever changes).

Also prints a summary: for every object id, whether ANY of its placements (on any track) have a
non-zero setting anywhere -- the fastest way to answer "is this settings-driven or hardcoded?"
without opening the JSON.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import brres_to_glb as b
import export_objects as eo

OUT_DIR = os.path.join(HERE, 'data')
OUT_FILE = os.path.join(OUT_DIR, 'all_object_placements.json')

# The 32 real race-track folders web/assets/tracks uses (battle stages/demos excluded -- the web
# player doesn't load them). Keep in sync with web/assets/tracks/*/course.szs if that ever changes.
TRACKS = [
    'beginner_course', 'farm_course', 'kinoko_course', 'factory_course', 'castle_course',
    'shopping_course', 'boardcross_course', 'truck_course', 'senior_course', 'ridgehighway_course',
    'treehouse_course', 'koopa_course', 'rainbow_course', 'volcano_course', 'desert_course',
    'water_course', 'old_peach_gc', 'old_mario_gc', 'old_waluigi_gc', 'old_donkey_gc',
    'old_falls_ds', 'old_desert_ds', 'old_garden_ds', 'old_town_ds', 'old_mario_sfc',
    'old_obake_sfc', 'old_mario_64', 'old_sherbet_64', 'old_koopa_64', 'old_donkey_64',
    'old_koopa_gba', 'old_heyho_gba',
]


def main():
    mkwii = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'MKWii')
    course_dir = os.path.join(mkwii, 'Race', 'Course')

    common_data = b.yaz0_decompress(open(os.path.join(mkwii, 'Race', 'Common.szs'), 'rb').read())
    common = b.u8_files(common_data)
    flow = eo.read_obj_flow(common)

    os.makedirs(OUT_DIR, exist_ok=True)
    out = {}
    seen_nonzero = {}   # id -> set of nonzero setting values seen, for the summary

    for folder in TRACKS:
        path = os.path.join(course_dir, folder + '.szs')
        if not os.path.exists(path):
            print('  ! missing %s' % path)
            continue
        data = b.yaz0_decompress(open(path, 'rb').read())
        files = b.u8_files(data)
        placements = []
        for g in eo.read_gobj(files['course.kmp']):
            if not (g['presence'] & 1):
                continue
            name, resources = flow.get(g['id'], ('id_%d' % g['id'], []))
            placements.append({
                'id': g['id'], 'name': name, 'resources': resources,
                'pos': g['pos'], 'rot': g['rot'], 'scale': g['scale'], 'settings': g['settings'],
            })
            if any(g['settings']):
                seen_nonzero.setdefault(g['id'], set()).add(tuple(g['settings']))
        out[folder] = placements
        print('%-22s %4d placements' % (folder, len(placements)))

    with open(OUT_FILE, 'w') as f:
        json.dump(out, f, indent=1)
    print('\nwrote %s' % OUT_FILE)

    print('\nObject ids with at least one non-zero setting somewhere, on any track:')
    for oid in sorted(seen_nonzero):
        name = flow.get(oid, ('id_%d' % oid,))[0]
        print('  %-20s (id %d): %d distinct settings tuple(s)' % (name, oid, len(seen_nonzero[oid])))


if __name__ == '__main__':
    main()
