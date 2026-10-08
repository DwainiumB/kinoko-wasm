"""Export the game's race particle effects (Race/Common.szs: Effect/RKRace.breff + .breft) for the web player.

Usage: python tools/export_effects.py [path/to/MKWii]

Writes web/assets/effects/effects.json (every effect: emitter, particle and animation-curve data) and
web/assets/effects/tex/<name>.png (the particle textures). Nintendo assets: git-ignored.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import brres_to_glb as b  # noqa: E402
import eft  # noqa: E402

OUT = os.path.join(HERE, '..', 'web', 'assets', 'effects')


def rnd(v, n=5):
    if isinstance(v, float):
        return round(v, n)
    if isinstance(v, (list, tuple)):
        return [rnd(x, n) for x in v]
    return v


def export_pair(data, tex, OUT):
    """One .breff + .breft pair -> OUT/effects.json + OUT/tex/*.png."""
    os.makedirs(os.path.join(OUT, 'tex'), exist_ok=True)

    infos = eft.read_breft(tex)
    for name, info in infos.items():
        img = eft.decode_breft(tex, info)
        if info['fmt'] in (0, 1):      # I4 / I8: the GX alpha of an intensity texture is its intensity
            img.putalpha(img.convert('L'))
        img.save(os.path.join(OUT, 'tex', name.replace('#', '_') + '.png'))
    print('%d textures' % len(infos))

    table = eft.read_effect_table(data)
    out = {}
    for name, (o, size) in table.items():
        e = eft.emitter(data, o + 8)
        po = o + 8 + 0x14C
        psz = struct.unpack_from('>I', data, po)[0]
        q = eft.particle(data, po)
        (pa, ea), pinit, einit = eft.animations(data, po + psz + 4, o + size)
        anims = []
        for i, a in enumerate(pa + ea):
            d = {'p': 1 if i < len(pa) else 0, 'init': 1 if i < pinit and i < len(pa) else 0, 'curve': a['curve'], 'kind': a['kind'],
                 'comps': a['comps'], 'frames': a['frames'], 'proc': a['proc'], 'loop': a['loop']}
            if a['keys'] is not None:
                d['keys'] = [[k[0], rnd(k[1])] for k in a['keys']]
            if a['samples'] is not None:
                d['samples'] = rnd(a['samples'])
            if a.get('range'):
                d['range'] = rnd(a['range'])
            if a['names']:
                d['names'] = a['names']
            if a['infoRaw']:
                d['info'] = a['infoRaw']
            if a['curve'] == 5:      # child events: 24-byte keys {u16 frame ... setting @12: s16 speed, scale, alpha, color, weight, type, flag; name index @23}
                kr = bytes.fromhex(a['keyRaw'])
                n = struct.unpack_from('>H', kr, 0)[0]
                d['children'] = [{'frame': struct.unpack_from('>H', kr, 4 + 24 * i)[0],
                                  'speed': struct.unpack_from('>h', kr, 4 + 24 * i + 12)[0], 'scale': kr[4 + 24 * i + 14],
                                  'alpha': kr[4 + 24 * i + 15], 'color': kr[4 + 24 * i + 16], 'weight': kr[4 + 24 * i + 17],
                                  'type': kr[4 + 24 * i + 18], 'flag': kr[4 + 24 * i + 19], 'name': kr[4 + 24 * i + 23]} for i in range(n)]
            anims.append(d)
        out[name] = {'e': {k: rnd(v) for k, v in e.items()}, 'p': {k: rnd(v) for k, v in q.items() if k != 'end'}, 'a': anims}
    with open(os.path.join(OUT, 'effects.json'), 'w') as f:
        json.dump({'effects': out, 'textures': {n: {'w': i['w'], 'h': i['h'], 'fmt': i['fmt']} for n, i in infos.items()}}, f, separators=(',', ':'))
    print('%d effects -> %s (%.0f KB)' % (len(out), os.path.abspath(OUT), os.path.getsize(os.path.join(OUT, 'effects.json')) / 1024))


def main():
    args = sys.argv[1:]
    course = None
    if '--course' in args:      # a course's own effects (fire poles, lava bombs ...): one folder per effect file
        i = args.index('--course')
        course = args[i + 1]
        del args[i:i + 2]
    mk = args[0] if args else r'C:\Users\dwain\MKWii'
    if course:
        files = b.u8_files(b.yaz0_decompress(open(os.path.join(mk, 'Race', 'Course', course + '.szs'), 'rb').read()))
        for k in sorted(files):
            if k.endswith('.breff'):
                export_pair(files[k], files[k[:-5] + 'breft'], os.path.join(HERE, '..', 'web', 'assets', 'tracks', course, 'effects', k.split('/')[1]))
        return
    files = b.u8_files(b.yaz0_decompress(open(os.path.join(mk, 'Race', 'Common.szs'), 'rb').read()))
    export_pair(files['Effect/RKRace.breff'], files['Effect/RKRace.breft'], OUT)

if __name__ == '__main__':
    main()
