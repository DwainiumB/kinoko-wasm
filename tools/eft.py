"""Reader for the game's particle effects: Effect/RKRace.breff (+ .breft textures) in Race/Common.szs.

A BREFF is a table of effects (names 'rk_driftSpark1L_Spark00', ...). Each effect is one emitter with its
particle settings and animation tables. Layout (big endian), based on the mkwiiki 'BREFF' page and checked
against the file:
    0x10 'REFF' block, project header, then a table of [u16 len][name\0][u32 offset][u32 size]
    data of an effect = base 0x30 + offset:  u32 0, u32 emitterSize, emitter (0x14C), particle, animations
"""
import re
import struct

BASE = 0x30


def read_effect_table(b):
    """{effect name: (offset, size)}: the entry table is a run of (u16 length, NUL-terminated name, u32 offset,
    u32 size); the longest such run starting at any 'rk_' name is the table (a course file's project name comes first)."""
    best = {}
    i = b.find(b'rk_')
    while i >= 0:
        p, out = i - 2, {}
        while p >= 0 and p + 2 < len(b):
            l = struct.unpack_from('>H', b, p)[0]
            if not (2 <= l <= 64) or p + 1 + l >= len(b) or not all(32 <= c < 127 for c in b[p + 2:p + 1 + l]) or b[p + 1 + l] != 0 or p + 2 + l + 8 > len(b):
                break
            name = b[p + 2:p + 1 + l].decode()
            off, size = struct.unpack_from('>II', b, p + 2 + l)
            out[name] = (0x18 + struct.unpack_from('>I', b, 0x18)[0] + off, size)   # offsets count from the end of the project header
            p += 2 + l + 8
        if len(out) > len(best):
            best = out
        i = b.find(b'rk_', i + 1)
    return best


def emitter(b, o):
    """Emitter fields at o (start of the 0x14C block)."""
    u8 = lambda k: b[o + k]
    u16 = lambda k: struct.unpack_from('>H', b, o + k)[0]
    f = lambda k, n=1: struct.unpack_from('>%df' % n, b, o + k)
    return {
        'flags': struct.unpack_from('>I', b, o)[0], 'emitFlags': b[o + 4:o + 7].hex(), 'shape': u8(7),
        'emitterLife': u16(8), 'particleLife': u16(0xA), 'lifeRandom': u8(0xC),
        'intervalRandom': u8(0xE), 'emitRandom': u8(0xF), 'rate': f(0x10)[0],
        'emitStart': u16(0x14), 'emitEnd': u16(0x16), 'interval': u16(0x18),
        'dims': f(0x1C, 6), 'diversion': u16(0x34), 'velRandom': u8(0x36), 'momentumRandom': u8(0x37),
        'powerRadiation': f(0x38)[0], 'powerY': f(0x3C)[0], 'powerRandom': f(0x40)[0],
        'powerNormal': f(0x44)[0], 'diffusionNormal': f(0x48)[0], 'powerSpec': f(0x4C)[0], 'diffusionSpec': f(0x50)[0],
        'emitAngle': f(0x54, 3), 'scale': f(0x60, 3), 'rot': f(0x6C, 3), 'trans': f(0x78, 3),
        'drawFlags': u16(0x94), 'tevStages': u8(0x99), 'blend': (u8(0xF0), u8(0xF1), u8(0xF2), u8(0xF3)),
        'ptype': u8(0x140), 'ptypeOpt': u8(0x141), 'moveDir': u8(0x142), 'rotAxis': u8(0x143), 'zoffset': f(0x148)[0],
        'alphaFlick': (u8(0x105), u16(0x106), u8(0x108)), 'lighting': (u8(0x109), u8(0x10A)),
        # TEV: texture per stage, colour/alpha args (a b c d), colour/alpha ops (op bias scale clamp outReg), konst selectors
        'tevTex': list(b[o + 0x9C:o + 0xA0]),
        'tevColor': [list(b[o + 0xA0 + 4 * i:o + 0xA4 + 4 * i]) for i in range(4)],
        'tevColorOp': [list(b[o + 0xB0 + 5 * i:o + 0xB5 + 5 * i]) for i in range(4)],
        'tevAlpha': [list(b[o + 0xC4 + 4 * i:o + 0xC8 + 4 * i]) for i in range(4)],
        'tevAlphaOp': [list(b[o + 0xD4 + 5 * i:o + 0xD9 + 5 * i]) for i in range(4)],
        'kColorSel': list(b[o + 0xE8:o + 0xEC]), 'kAlphaSel': list(b[o + 0xEC:o + 0xF0]),
        'colorInput': list(b[o + 0xF4:o + 0xFC]), 'alphaInput': list(b[o + 0xFC:o + 0x104]),
        'zcomp': u8(0x104), 'alphaCmp': (u8(0x96), u8(0x97), u8(0x98)), 'alphaCmpFirst': u8(0x96),
        'texPivot': (struct.unpack_from('>b', b, o + 0x13D)[0], struct.unpack_from('>b', b, o + 0x13E)[0]),
        # fields the engine needs beyond the above (names follow nw4r::ef::EmitterDesc)
        'emitFlag': struct.unpack_from('>I', b, o + 4)[0], 'commonFlag': struct.unpack_from('>I', b, o)[0],
        'ptclLifeRandom': struct.unpack_from('>b', b, o + 0xC)[0], 'inheritChildPtclTranslate': struct.unpack_from('>b', b, o + 0xD)[0],
        'emitIntervalRandom': struct.unpack_from('>b', b, o + 0xE)[0], 'emitRandom': struct.unpack_from('>b', b, o + 0xF)[0],
        'emitPast': u16(0x16), 'inheritPtclTranslate': struct.unpack_from('>b', b, o + 0x1A)[0],
        'inheritChildEmitTranslate': struct.unpack_from('>b', b, o + 0x1B)[0],
        'velInitVelocityRandom': struct.unpack_from('>b', b, o + 0x36)[0], 'velMomentumRandom': struct.unpack_from('>b', b, o + 0x37)[0],
        'lod': (u8(0x84), u8(0x85), u8(0x86), u8(0x87)), 'randomSeed': struct.unpack_from('>I', b, o + 0x88)[0],
        'typeOption': u8(0x141), 'typeDir': u8(0x142), 'typeAxis': u8(0x143), 'typeOptions': (u8(0x144), u8(0x145), u8(0x146)),
    }


def particle(b, o):
    """Particle block at o."""
    size = struct.unpack_from('>I', b, o)[0]
    col = [tuple(b[o + 4 + 4 * k:o + 8 + 4 * k]) for k in range(4)]
    f = lambda k, n: struct.unpack_from('>%df' % n, b, o + k)
    q = {'size': size, 'color1A': col[0], 'color1B': col[1], 'color2A': col[2], 'color2B': col[3],
         'psize': f(0x14, 2), 'pscale': f(0x1C, 2), 'prot': f(0x24, 3),
         'tscale': [f(0x30, 2), f(0x38, 2), f(0x40, 2)], 'trot': f(0x48, 3),
         'ttrans': [f(0x54, 2), f(0x5C, 2), f(0x64, 2)], 'wrap': struct.unpack_from('>H', b, o + 0x78)[0],
         'texReverse': b[o + 0x7A], 'alphaRef': (b[o + 0x7B], b[o + 0x7C]), 'rotOffset': f(0x80, 3),
         'rotOffsetRandom': (b[o + 0x7D], b[o + 0x7E], b[o + 0x7F])}
    p = o + 0x8C
    tex = []
    for _ in range(3):
        l = struct.unpack_from('>H', b, p)[0]
        tex.append(b[p + 2:p + 1 + l].decode() if l > 1 else '')
        p += 2 + l
    q['tex'] = tex
    q['end'] = p
    return q


CURVE_NAMES = {0: 'byte', 3: 'float', 6: 'rotate', 4: 'tex', 5: 'child', 7: 'field', 2: 'postfield', 11: 'emitter'}
KIND_BYTEFLOAT = {0: 'color0Pri', 3: 'alpha0Pri', 4: 'color0Sec', 7: 'alpha0Sec', 8: 'color1Pri', 11: 'alpha1Pri', 12: 'color1Sec',
                  15: 'alpha1Sec', 16: 'size', 24: 'scale', 44: 'tex1Scale', 68: 'tex1Rot', 80: 'tex1Trans', 52: 'tex2Scale',
                  72: 'tex2Rot', 88: 'tex2Trans', 60: 'texIndScale', 76: 'texIndRot', 96: 'texIndTrans', 119: 'alphaRef0'}
KIND_FIELD = {0: 'gravity', 1: 'speed', 2: 'magnet', 3: 'newton', 4: 'vortex', 6: 'spin', 7: 'random', 8: 'tail'}
KIND_EMITTER = {44: 'common', 124: 'scale', 136: 'rotate', 112: 'translate', 72: 'speedOrig', 76: 'speedY', 80: 'speedRandom',
                84: 'speedNormal', 92: 'speedSpec', 8: 'emission'}
KIND_POST = {0: 'size', 12: 'rotate', 24: 'translate'}


def animations(b, o, end):
    """Animation tables starting at o (right after the particle block). Returns (particle anims, emitter anims);
    an anim = dict(kind, curve, enable, frames, flags, keys=[(frame, [values...])], ranges, ...)."""
    a = struct.unpack_from('>H', b, o)[0]
    ptcl_init = struct.unpack_from('>H', b, o + 2)[0]
    sizes = list(struct.unpack_from('>%dI' % a, b, o + 4 + 4 * a))
    p = o + 4 + 8 * a
    n_emit, emit_init = struct.unpack_from('>HH', b, p)
    esizes = list(struct.unpack_from('>%dI' % n_emit, b, p + 4 + 4 * n_emit))
    p += 4 + 8 * n_emit
    out = ([], [])
    for lst, szs in ((out[0], sizes), (out[1], esizes)):
        for sz in szs:
            lst.append(anim(b, p, sz))
            p += sz
    return out, ptcl_init, emit_init


def anim(b, o, size):
    magic, kind, curve, enable, proc, loop, seed, frames = struct.unpack_from('>BBBBBBHH', b, o)
    ks, rs, rn, ns, inf = struct.unpack_from('>5I', b, o + 0xC)
    q = o + 0x20
    comps = [i for i in range(8) if enable >> i & 1]
    nc = len(comps)
    vs = {0: 1, 3: 4, 6: 4}.get(curve)            # byte (colours, alpha), float, rotation (float radians)
    d = {'kind': kind, 'curve': curve, 'enable': enable, 'comps': comps, 'frames': frames, 'proc': proc, 'loop': loop,
         'seed': seed, 'keys': None, 'samples': None}
    key = b[q:q + ks]
    if vs and ks:
        stride = 12 + nc * vs
        stride += stride & 1
        count = struct.unpack_from('>H', key, 0)[0]
        pad4 = lambda n: (n + 3) & ~3
        if ks == pad4(4 + count * stride):           # keyframes: u16 frame, pad, 8 bytes (interpolation), values
            keys = []
            for i in range(count):
                r = 4 + i * stride
                fr = struct.unpack_from('>H', key, r)[0]
                if vs == 4:
                    v = list(struct.unpack_from('>%df' % nc, key, r + 12))
                else:
                    v = list(key[r + 12:r + 12 + nc])
                keys.append((fr, v, key[r + 2:r + 12].hex()))
            d['keys'] = keys
        elif ks == pad4(frames * nc * vs):            # one value per frame, no header
            fmt = '>%d%s' % (frames * nc, 'f' if vs == 4 else 'B')
            v = struct.unpack_from(fmt, key, 0)
            d['samples'] = [list(v[i * nc:(i + 1) * nc]) for i in range(frames)]
    d['keyRaw'] = key.hex()
    q += ks
    # range table: u16 count, pad, then per enabled component (float curves: two floats; byte curves: two bytes)
    rng = b[q:q + rs]
    d['rangeRaw'] = rng.hex()
    if rs > 4 and vs:
        if vs == 4:
            d['range'] = [list(struct.unpack_from('>2f', rng, 4 + 8 * i)) for i in range(nc) if 4 + 8 * i + 8 <= rs]
        elif curve == 0:
            d['range'] = [list(rng[4 + 2 * i:6 + 2 * i]) for i in range(nc) if 6 + 2 * i <= rs]
    q += rs
    d['randRaw'] = b[q:q + rn].hex()
    q += rn
    names = []
    if ns > 4:
        cnt = struct.unpack_from('>H', b, q)[0]
        r = q + 4 + 4 * cnt
        for _ in range(cnt):
            l = struct.unpack_from('>H', b, r)[0]
            names.append(b[r + 2:r + 1 + l].decode('latin1'))
            r += 2 + l
    d['names'] = names
    q += ns
    d['infoRaw'] = b[q:q + inf].hex()
    return d


# ---------------------------------------------------------------------------------------------
# BREFT textures
# ---------------------------------------------------------------------------------------------

def read_breft(t):
    """{name: dict(width, height, format, mipmaps, data offset...)} of a BREFT."""
    # 'REFT' block at 0x10: u32 size, u32 dataOffset (from 0x18) -> table at 0x18 + dataOffset
    data_off = struct.unpack_from('>i', t, 0x18)[0]
    tbl = 0x18 + data_off
    n = struct.unpack_from('>H', t, tbl + 4)[0]
    p = tbl + 8
    out = {}
    for _ in range(n):
        l = struct.unpack_from('>H', t, p)[0]
        name = t[p + 2:p + 1 + l].decode('latin1')
        off, ln = struct.unpack_from('>II', t, p + 2 + l)
        h = tbl + off
        w, hh = struct.unpack_from('>HH', t, h + 4)
        imglen = struct.unpack_from('>I', t, h + 8)[0]
        fmt, pfmt, ncol = t[h + 0xC], t[h + 0xD], struct.unpack_from('>H', t, h + 0xE)[0]
        out[name] = {'w': w, 'h': hh, 'fmt': fmt, 'pfmt': pfmt, 'ncol': ncol, 'imglen': imglen, 'off': h + 0x20,
                     'pal': h + 0x20 + imglen, 'mips': t[h + 0x14]}
        p += 2 + l + 8
    return out


def decode_breft(t, info):
    """Decode one BREFT texture (top mip level) to a PIL RGBA image."""
    import numpy as np
    from PIL import Image
    import brres_to_glb as B
    fmt, w, h = info['fmt'], info['w'], info['h']
    if fmt in (8, 9, 10):        # palette formats: C4, C8, C14X2
        bw, bh, bits = {8: (8, 8, 4), 9: (8, 4, 8), 10: (4, 4, 16)}[fmt]
        pw, ph = -(-w // bw) * bw, -(-h // bh) * bh
        raw = np.frombuffer(t, np.uint8, pw * ph * bits // 8, info['off'])
        if fmt == 8:
            idx = np.stack([raw >> 4, raw & 15], -1).reshape(-1)
        elif fmt == 9:
            idx = raw
        else:
            idx = (raw.view('>u2') & 0x3FFF)
        pal = np.frombuffer(t, '>u2', info['ncol'], info['pal']).astype(np.uint32)
        if info['pfmt'] == 0:    # IA8
            c = np.stack([pal & 255] * 3 + [pal >> 8], -1)
        elif info['pfmt'] == 1:  # RGB565
            c = np.concatenate([B._rgb565(pal), np.full(pal.shape + (1,), 255, np.uint8)], -1)
        else:                    # RGB5A3
            opaque = pal >> 15 == 1
            c5 = lambda s: (pal >> s & 31) * 255 // 31
            c4 = lambda s: (pal >> s & 15) * 17
            c = np.stack([np.where(opaque, c5(10), c4(8)), np.where(opaque, c5(5), c4(5 - 1)), np.where(opaque, c5(0), c4(0)),
                          np.where(opaque, 255, (pal >> 12 & 7) * 255 // 7)], -1)
        pix = np.asarray(c, np.uint8)[np.minimum(idx.astype(np.int64), len(pal) - 1)]
        pix = pix.reshape((pw // bw) * (ph // bh), bh, bw, 4)
        img = B._unblock(pix, w, h, bw, bh, pw, ph)
        return Image.fromarray(np.ascontiguousarray(img), 'RGBA')
    # everything else: reuse the TEX0 decoder with a fake TEX0 header
    head = bytearray(0x40)
    struct.pack_into('>i', head, 0x10, 0x40)
    struct.pack_into('>HH', head, 0x1C, w, h)
    struct.pack_into('>I', head, 0x20, fmt)
    r = B.Reader(bytes(head) + t[info['off']:info['off'] + info['imglen']])
    return B.decode_texture(r, 0)
