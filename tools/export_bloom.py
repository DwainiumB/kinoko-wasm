"""Export each course's bloom settings (posteffect/posteffect.bblm, 'PBLM' v1) for the web player.

Usage: python tools/export_bloom.py [path/to/MKWii]
Writes web/assets/tracks/<course>/bloom.json. Field layout as in noclip's MarioKartWii/PostEffect.ts.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import brres_to_glb as b  # noqa: E402
import export_tracks  # noqa: E402


def parse_bblm(d):
    assert d[:4] == b'PBLM' and d[8] == 1
    f32 = lambda o: struct.unpack_from('>f', d, o)[0]
    rgba = lambda o: [x / 255 for x in d[o:o + 4]]
    return {'thresholdAmount': f32(0x10), 'thresholdColor': rgba(0x14), 'compositeColor': rgba(0x18),
            'blurFlags': struct.unpack_from('>H', d, 0x1C)[0],
            'blur0Radius': f32(0x20), 'blur0Intensity': f32(0x24), 'blur1Radius': f32(0x40), 'blur1Intensity': f32(0x44),
            'compositeBlendMode': d[0x80], 'blur1NumPasses': d[0x81], 'colorScale0': f32(0x9C), 'colorScale1': f32(0xA0)}


def main():
    mk = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\dwain\MKWii'
    for folder in export_tracks.RACE_TRACKS:
        src = os.path.join(mk, 'Race', 'Course', folder + '.szs')
        if not os.path.exists(src):
            continue
        files = b.u8_files(b.yaz0_decompress(open(src, 'rb').read()))
        d = files.get('posteffect/posteffect.bblm')
        if d is None:
            print('%-20s no bloom' % folder)
            continue
        out = os.path.join(export_tracks.TRACKS_OUT, folder)
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, 'bloom.json'), 'w') as f:
            json.dump(parse_bblm(d), f)
        print('%-20s ok' % folder)


if __name__ == '__main__':
    main()
