"""Cross-reference every object id actually PLACED across all 32 race tracks (from
dump_all_object_settings.py's output) against what ObjectDirector::createObject() really does with
that id in Kinoko's C++ source -- a real, specific class; an explicit no-behavior stub
(ObjectCollidable/ObjectKCL, just sits there with plain collision, no per-frame behavior); or
completely unimplemented (falls to `default: ObjectNoImpl`, meaning Kinoko doesn't even know it
exists as anything other than inert geometry).

This turns "some objects are still wrong" into a concrete, prioritized list instead of guessing --
sorted by total placement count, so the objects most likely to actually matter (appear often, on
many tracks) surface first. A "real class" verdict here does NOT mean the class's behavior is
verified correct (see kinokoT1: it had a real class, and was still wrong) -- it only screens out
the objects that couldn't possibly be right yet, because nothing decides their behavior at all.

Usage:
    python tools/audit_object_impl.py

Requires tools/data/all_object_placements.json (run dump_all_object_settings.py first).

Output: tools/data/object_impl_audit.json (the full per-id table) + a sorted summary on stdout.
"""

import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(HERE, 'data')
PLACEMENTS_FILE = os.path.join(DATA_DIR, 'all_object_placements.json')
OUT_FILE = os.path.join(DATA_DIR, 'object_impl_audit.json')

OBJECT_ID_HH = os.path.join(HERE, '..', 'source', 'game', 'field', 'obj', 'ObjectId.hh')
OBJECT_DIRECTOR_CC = os.path.join(HERE, '..', 'source', 'game', 'field', 'ObjectDirector.cc')


def parse_object_id_enum():
    """{enum name: numeric id} from the real ObjectId enum (ObjectId.hh) -- only the main
    ObjectId enum, stops at the BlacklistedObjectId enum that follows it in the same file."""
    text = open(OBJECT_ID_HH).read()
    body = text.split('enum class ObjectId {', 1)[1].split('};', 1)[0]
    ids = {}
    for m in re.finditer(r'(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)', body):
        ids[m.group(1)] = int(m.group(2), 0)
    return ids


def parse_create_object():
    """{numeric id: verdict}. verdict is one of:
    ('real', ClassName) -- a specific, real implementation
    ('stub', 'ObjectCollidable'|'ObjectKCL') -- explicit no-behavior stub
    (id not present at all -- caller treats that as unimplemented/default ObjectNoImpl)
    """
    name_to_id = parse_object_id_enum()
    text = open(OBJECT_DIRECTOR_CC).read()
    body = text.split('ObjectDirector::createObject(', 1)[1]
    body = body.split('switch (id) {', 1)[1].split('\n    }\n}', 1)[0]

    verdicts = {}
    pending_names = []
    for line in body.splitlines():
        line = line.strip()
        m_case = re.match(r'case ObjectId::(\w+):$', line)
        if m_case:
            pending_names.append(m_case.group(1))
            continue
        m_ret = re.match(r'return EGG::egg_new<(\w+)>\(params\);', line)
        if m_ret and pending_names:
            cls = m_ret.group(1)
            kind = 'stub' if cls in ('ObjectCollidable', 'ObjectKCL') else 'real'
            for n in pending_names:
                if n in name_to_id:
                    verdicts[name_to_id[n]] = (kind, cls)
            pending_names = []
    return verdicts


def main():
    if not os.path.exists(PLACEMENTS_FILE):
        raise SystemExit('Missing %s -- run dump_all_object_settings.py first.' % PLACEMENTS_FILE)
    placements = json.load(open(PLACEMENTS_FILE))
    verdicts = parse_create_object()

    counts = {}   # id -> {name, total, tracks: [folder, ...]}
    for folder, plist in placements.items():
        for p in plist:
            c = counts.setdefault(p['id'], {'name': p['name'], 'total': 0, 'tracks': set()})
            c['total'] += 1
            c['tracks'].add(folder)

    rows = []
    for oid, c in counts.items():
        kind, cls = verdicts.get(oid, ('unimplemented', 'ObjectNoImpl'))
        rows.append({
            'id': oid, 'name': c['name'], 'total_placements': c['total'],
            'tracks': sorted(c['tracks']), 'status': kind, 'class': cls,
        })
    rows.sort(key=lambda r: (-r['total_placements'], r['name']))

    with open(OUT_FILE, 'w') as f:
        json.dump(rows, f, indent=1)
    print('wrote %s (%d distinct placed object ids)\n' % (OUT_FILE, len(rows)))

    for label, kind in [('UNIMPLEMENTED (falls to default ObjectNoImpl -- no behavior at all)', 'unimplemented'),
                         ('STUB (explicit ObjectCollidable/ObjectKCL -- static collision only, no per-frame behavior)', 'stub'),
                         ('real class (screened in, NOT a correctness guarantee)', 'real')]:
        subset = [r for r in rows if r['status'] == kind]
        total_placements = sum(r['total_placements'] for r in subset)
        print('=== %s: %d ids, %d placements ===' % (label, len(subset), total_placements))
        if kind != 'real':
            for r in subset:
                print('  %-22s id 0x%-4x  %3d placements on %2d tracks: %s' % (
                    r['name'], r['id'], r['total_placements'], len(r['tracks']), ', '.join(r['tracks'][:6]) + ('...' if len(r['tracks']) > 6 else '')))
        print()


if __name__ == '__main__':
    main()
