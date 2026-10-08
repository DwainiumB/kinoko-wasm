"""Export each course's depth-of-field settings (posteffect/posteffect.bdof, 'PDOF' v0) and warp texture
(posteffect.bti, GX IA8) for the web player.

Usage: python tools/export_dof.py [path/to/MKWii]
Writes web/assets/tracks/<course>/dof.json and, where the course has one, dof_warp.png (r=g=b=intensity, a=alpha).
Field layout as in noclip's MarioKartWii/PostEffect.ts.
"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import brres_to_glb as b  # noqa: E402
import export_tracks  # noqa: E402


def parse_bdof(d):
    assert d[:4] == b'PDOF' and d[8] == 0
    f32 = lambda o: struct.unpack_from('>f', d, o)[0]
    return {'flags': struct.unpack_from('>H', d, 0x10)[0], 'blurAlpha': [d[0x12], d[0x13]], 'drawMode': d[0x14],
            'blurDrawAmount': d[0x15], 'depthCurveType': d[0x16], 'focusCenter': f32(0x18), 'focusRange': f32(0x1C),
            'blurRadius': f32(0x24), 'scrollS': f32(0x28), 'scrollT': f32(0x2C), 'indScaleS': f32(0x30), 'indScaleT': f32(0x34),
            'scaleS': f32(0x38), 'scaleT': f32(0x3C)}


def decode_bti_ia8(d):
    fmt, w, h = d[0], struct.unpack_from('>H', d, 2)[0], struct.unpack_from('>H', d, 4)[0]
    assert fmt == 3, 'only IA8 warp textures are handled (format %d)' % fmt
    off = struct.unpack_from('>I', d, 0x1C)[0]
    out = np.zeros((h, w, 4), np.uint8)
    p = off
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = np.frombuffer(d, np.uint8, 32, p).reshape(4, 4, 2)   # per pixel: alpha byte, then intensity byte
            p += 32
            out[by:by + 4, bx:bx + 4, 0] = out[by:by + 4, bx:bx + 4, 1] = out[by:by + 4, bx:bx + 4, 2] = blk[:, :, 1]
            out[by:by + 4, bx:bx + 4, 3] = blk[:, :, 0]
    return Image.fromarray(out, 'RGBA')


def main():
    mk = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\dwain\MKWii'
    for folder in export_tracks.RACE_TRACKS:
        src = os.path.join(mk, 'Race', 'Course', folder + '.szs')
        if not os.path.exists(src):
            continue
        files = b.u8_files(b.yaz0_decompress(open(src, 'rb').read()))
        d = files.get('posteffect/posteffect.bdof')
        if d is None:
            print('%-20s no dof' % folder)
            continue
        out = os.path.join(export_tracks.TRACKS_OUT, folder)
        os.makedirs(out, exist_ok=True)
        cfg = parse_bdof(d)
        bti = files.get('posteffect/posteffect.bti')
        if bti is not None:
            decode_bti_ia8(bti).save(os.path.join(out, 'dof_warp.png'))
            cfg['warp'] = 'dof_warp.png'
        with open(os.path.join(out, 'dof.json'), 'w') as f:
            json.dump(cfg, f)
        print('%-20s mode %d  flags %#x%s' % (folder, cfg['drawMode'], cfg['flags'], '  warp' if bti is not None else ''))


if __name__ == '__main__':
    main()
