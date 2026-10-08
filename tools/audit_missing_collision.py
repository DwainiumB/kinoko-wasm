"""Narrows audit_object_impl.py's 137 "unimplemented" object ids down to the ones that actually
matter: does a real per-object .kcl collision file (from dump_all_kcl.py's output) exist for it in
the SAME track archive it's placed on? If so, the real game almost certainly collides with it (why
else would a .kcl file for it exist), and Kinoko currently gives it none at all (ObjectNoImpl has no
collision) -- a kart should be able to hit/bounce off/drive on it but currently just clips through.
If no matching .kcl exists anywhere, it's most likely a real static decoration (sound trigger,
lens flare, Mii sign, distant scenery) that never needed object-level behavior in the first place,
and being unimplemented in Kinoko is correct, not a bug.

The name match is the same heuristic dump_all_kcl.py already flags as unverified (an object's real
getKclName() can diverge from its resource name) -- treat a match here as "investigate this one
first", not proof, and treat a non-match as "probably fine, but only probably".

Usage (after dump_all_object_settings.py, dump_all_kcl.py, and audit_object_impl.py have all been
run at least once):
    python tools/audit_missing_collision.py

Output: tools/data/missing_collision_candidates.json + a sorted summary on stdout.
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(HERE, 'data')


def main():
    audit = json.load(open(os.path.join(DATA_DIR, 'object_impl_audit.json')))
    placements = json.load(open(os.path.join(DATA_DIR, 'all_object_placements.json')))
    kcl_files = json.load(open(os.path.join(DATA_DIR, 'all_kcl_files.json')))

    # id -> {resources} used anywhere (name-matching candidates), from the placements dump
    resources_by_id = {}
    for folder, plist in placements.items():
        for p in plist:
            resources_by_id.setdefault(p['id'], set()).update([p['name']] + p.get('resources', []))

    candidates = []
    for row in audit:
        if row['status'] != 'unimplemented':
            continue
        names = resources_by_id.get(row['id'], set())
        matches = []   # (track, kcl_filename, triangles, types)
        for track in row['tracks']:
            stems = {os.path.splitext(f)[0]: f for f in kcl_files.get(track, {})}
            for stem, fname in stems.items():
                if stem in names:
                    info = kcl_files[track][fname]
                    matches.append({'track': track, 'file': fname, 'triangles': info['triangles'], 'types': info['types']})
        if matches:
            candidates.append({**row, 'kcl_matches': matches})

    candidates.sort(key=lambda r: -r['total_placements'])
    with open(os.path.join(DATA_DIR, 'missing_collision_candidates.json'), 'w') as f:
        json.dump(candidates, f, indent=1)

    print('%d of the 137 unimplemented ids have a name-matching .kcl file somewhere -- likely real, currently-missing collision:\n' % len(candidates))
    for r in candidates:
        print('%-20s id 0x%-4x  %3d placements: %s' % (r['name'], r['id'], r['total_placements'], r['tracks']))
        for m in r['kcl_matches']:
            print('    %-12s %-20s %4d triangles  types: %s' % (m['track'], m['file'], m['triangles'], m['types']))


if __name__ == '__main__':
    main()
