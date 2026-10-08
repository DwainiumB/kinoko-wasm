"""Export the objects placed on a course (trees, signs, Goombas, pipes, ...) for the web player.

Called by tools/export_tracks.py for every course; can also be run alone:
    python tools/export_objects.py [path/to/MKWii] [--only folder_name ...]

For each course, in web/assets/tracks/<folder>/:
    objects.json   {models, flow, placements}: which model file each object id uses (from the game's
                   ObjFlow.bin), and every object placed in the course's course.kmp (position,
                   rotation, scale) that shows up in a one-player race
    obj/<name>.glb one model file per resource the course's objects use (models in game units;
                   a resource can hold several models, e.g. a high and low detail one)

Objects Kinoko simulates (anything with collision) are drawn live from the physics by the page;
this data places the rest (scenery) and provides the models. These are Nintendo's assets: they are
git-ignored and must not be redistributed.
"""

import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import brres_to_glb as b
import object_rig  # noqa: E402

TRACKS_OUT = os.path.join(HERE, '..', 'web', 'assets', 'tracks')
FLOW_ENTRY = 0x74
MKWII = r'C:\Users\dwain\MKWii'
EXTRA_COMMON = {'koopa_course': ['bombCore']}
EXTRA_SHARED = {'old_waluigi_gc': ['pakkun_f']}   # models from Race/Course/Object the game's code adds without an ObjFlow entry (Waluigi Stadium's pipe piranhas)


def read_obj_flow(common):
    """{object id: (name, [resource names])} from Race/Common.szs ObjFlow.bin."""
    d = common['ObjFlow.bin']
    count = struct.unpack_from('>H', d, 0)[0]
    cstr = lambda raw: raw.split(b'\0')[0].decode('ascii', 'replace')
    flow = {}
    for i in range(count):
        e = d[2 + i * FLOW_ENTRY: 2 + (i + 1) * FLOW_ENTRY]
        res = [r for r in cstr(e[0x22:0x62]).replace(';', ',').split(',') if r and r != '-']
        flow[struct.unpack_from('>H', e, 0)[0]] = (cstr(e[2:0x22]), res)
    return flow


def read_gobj(kmp):
    """Every GOBJ entry of a course.kmp: id, position, rotation (degrees), scale, presence flags."""
    section_count, header_size = struct.unpack_from('>HH', kmp, 8)
    for i in range(section_count):
        start = header_size + struct.unpack_from('>I', kmp, 0x10 + 4 * i)[0]
        if kmp[start:start + 4] != b'GOBJ':
            continue
        count = struct.unpack_from('>H', kmp, start + 4)[0]
        for j in range(count):
            e = kmp[start + 8 + 0x3C * j: start + 8 + 0x3C * (j + 1)]
            yield {
                'id': struct.unpack_from('>H', e, 0)[0],
                'pos': list(struct.unpack_from('>3f', e, 4)),
                'rot': list(struct.unpack_from('>3f', e, 16)),
                'scale': list(struct.unpack_from('>3f', e, 28)),
                'set0': struct.unpack_from('>H', e, 42)[0],
                'settings': list(struct.unpack_from('>8H', e, 42)),
                'presence': struct.unpack_from('>H', e, 58)[0],
            }


def read_areas(kmp, types=(1,)):
    """AREA entries (0x30 bytes) of the given types; type 1 = environment effects (cave spores, embers)."""
    section_count, header_size = struct.unpack_from('>HH', kmp, 8)
    out = []
    for i in range(section_count):
        start = header_size + struct.unpack_from('>I', kmp, 0x10 + 4 * i)[0]
        if kmp[start:start + 4] != b'AREA':
            continue
        for j in range(struct.unpack_from('>H', kmp, start + 4)[0]):
            e = start + 8 + 0x30 * j
            if kmp[e + 1] in types:
                out.append({'type': kmp[e + 1], 'mode': kmp[e], 'pos': list(struct.unpack_from('>3f', kmp, e + 4)),
                            'rot': list(struct.unpack_from('>3f', kmp, e + 16)), 'scale': list(struct.unpack_from('>3f', kmp, e + 28)),
                            'set1': struct.unpack_from('>H', kmp, e + 40)[0]})
    return out


def convert_object_model(data, out_path, shared_files=None):
    """One resource .brres -> glb with a node per model. Returns (models, triangles)."""
    r = b.Reader(data)
    contents = b.brres_contents(r)
    tex = b.brres_textures(r, contents)
    if not tex and shared_files:    # e.g. kinokoT1.brres has no textures of its own: it uses kinoko.brres's
        tex = {}
        for f, fdata in shared_files.items():
            if f.endswith('.brres') and fdata is not data:
                try:
                    fr = b.Reader(fdata)
                    tex.update(b.brres_textures(fr, b.brres_contents(fr)))
                except Exception:
                    pass
    srt = b.parse_srt0(r, contents)
    g = b.Gltf()
    clips = dict(contents.get('AnmChr(NW4R)', []))
    clip_names = set()
    models, tris = [], 0
    for name, off in contents.get('3DModels(NW4R)', []):
        if 'shadow' in name.lower():
            continue   # blob shadows; the page doesn't draw them
        try:
            m = b.Mdl0(r, off, name)
            bones = {bone.name for bone in m.bones}
            mine = {c: o for c, o in clips.items() if {t for t, _ in b.Reader.dict(r, o + r.s32(o + 0x10))} & bones}
            if mine:    # skeletal animation: skinned model + clips (see object_rig.py)
                done, err = object_rig.add_rigged_model(g, m, tex, r, mine, srt)
                if err > 1e-3:
                    print('      ! %s: rig differs from the game bones by %.4f' % (name, err))
                models.append(name)
                clip_names.update(done)
                continue
            prims = b.model_primitives(g, 'obj', m, tex, track=True, srt_map=srt)
        except Exception as e:
            print('      ! model %s: %r' % (name, e))
            continue
        if not prims:
            continue
        tris += sum(len(p[4]) for p in prims)
        g.add_node(name, prims)
        models.append(name)
    if models:
        g.save(out_path)
    return models, tris, sorted(clip_names)


def export_course_objects(files, out_dir, flow, shared_dir=None):
    """Write obj/*.glb and objects.json for one course archive's files. Returns a stats dict."""
    placements = []
    for g in read_gobj(files['course.kmp']):
        if g['presence'] & 1:   # shown in a one-player race, as Kinoko decides
            placements.append({k: g[k] for k in ('id', 'pos', 'rot', 'scale', 'set0', 'settings')})

    ids = sorted({p['id'] for p in placements})
    used = {i: flow[i] for i in ids if i in flow}
    resources = sorted({r for _, res in used.values() for r in res})

    obj_dir = os.path.join(out_dir, 'obj')
    os.makedirs(obj_dir, exist_ok=True)
    models, missing, empty = {}, [], []
    for res in resources:
        data = files.get(res + '.brres')
        if data is None and shared_dir and os.path.exists(os.path.join(shared_dir, res + '.brres')):
            data = open(os.path.join(shared_dir, res + '.brres'), 'rb').read()
        if data is None:
            missing.append(res)      # effects (particles, sounds) have no model
            continue
        try:
            names, tris, anims = convert_object_model(data, os.path.join(obj_dir, res + '.glb'), files)
        except Exception as e:
            print('    ! %s: %r' % (res, e))
            empty.append(res)
            continue
        if names:
            models[res] = {'file': 'obj/%s.glb' % res, 'models': names, 'clips': anims}
        else:
            empty.append(res)

    # models the game's code draws itself, from Race/Common.szs (Bowser's Castle's fireball explosion, bombCore)
    for res in EXTRA_COMMON.get(os.path.basename(os.path.normpath(out_dir)), []):
        names, tris, anims = convert_object_model(b.load_common(MKWII)[res + '.brres'], os.path.join(obj_dir, res + '.glb'))
        if names:
            models[res] = {'file': 'obj/%s.glb' % res, 'models': names, 'clips': anims}

    for res in EXTRA_SHARED.get(os.path.basename(os.path.normpath(out_dir)), []):
        fpath = os.path.join(shared_dir, res + '.brres')
        if shared_dir and os.path.exists(fpath):
            names, tris, anims = convert_object_model(open(fpath, 'rb').read(), os.path.join(obj_dir, res + '.glb'))
            if names:
                models[res] = {'file': 'obj/%s.glb' % res, 'models': names, 'clips': anims}

    manifest = {
        'models': models,
        'flow': {str(i): {'name': n, 'resources': res} for i, (n, res) in used.items()},
        'placements': placements,
        'areas': read_areas(files['course.kmp']),
    }
    with open(os.path.join(out_dir, 'objects.json'), 'w') as f:
        json.dump(manifest, f, separators=(',', ':'))
    return {'placed': len(placements), 'kinds': len(ids), 'models': len(models), 'noModel': missing, 'empty': empty}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mkwii', nargs='?', default=r'C:\Users\dwain\MKWii')
    ap.add_argument('--only', nargs='*')
    args = ap.parse_args()
    global MKWII
    MKWII = args.mkwii
    import export_tracks
    common = b.load_common(args.mkwii)
    flow = read_obj_flow(common)
    shared = os.path.join(args.mkwii, 'Race', 'Course', 'Object')
    for folder in export_tracks.RACE_TRACKS:
        if args.only and folder not in args.only:
            continue
        files = b.u8_files(b.yaz0_decompress(open(os.path.join(args.mkwii, 'Race', 'Course', folder + '.szs'), 'rb').read()))
        s = export_course_objects(files, os.path.join(TRACKS_OUT, folder), flow, shared)
        print('%-20s %3d placed, %2d kinds, %2d models  no model: %s%s' % (
            folder, s['placed'], s['kinds'], s['models'], ','.join(s['noModel']) or '-', ('  EMPTY: ' + ','.join(s['empty'])) if s['empty'] else ''))


if __name__ == '__main__':
    main()
