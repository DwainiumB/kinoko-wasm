"""Convert Mario Kart Wii kart/driver models (BRRES MDL0 + TEX0) to glTF binary (.glb).

Usage:
    python tools/brres_to_glb.py [path/to/MKWii]      (default: C:/Users/dwain/MKWii)

For every vehicle/character archive in <MKWii>/Race/Kart (e.g. ma_kart-mr.szs), writes
web/assets/karts/<vehicle>-<char>/kart.glb and driver.glb, plus web/assets/karts/karts.json
(bike handlebar placement from Race/Common.szs). Models are in game units. kart.glb has one
static node per part (body, tire_fl, ...; the page places tires at the physics wheel
positions). driver.glb is a rigged model with all of the character's animations (drive,
drift_l/r, tricks, ...), placed at its seat offset; the page plays them and applies the
hand/foot IK stored in the file's extras (from kartDriverDispParam.bin). These files are
Nintendo's assets: they are git-ignored and must not be redistributed.

Also usable on a single file:  python tools/brres_to_glb.py --szs path/to/file.szs --out dir
"""

import argparse
import io
import base64
import json
import math
import os
import re
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'web', 'assets', 'karts')


# --------------------------------------------------------------------------------------------
# Archives: Yaz0 (SZS) + U8
# --------------------------------------------------------------------------------------------

def yaz0_decompress(data):
    if data[:4] != b'Yaz0':
        return data
    size = struct.unpack_from('>I', data, 4)[0]
    src, dst = 16, bytearray()
    while len(dst) < size:
        flags = data[src]
        src += 1
        for bit in range(8):
            if len(dst) >= size:
                break
            if flags & (0x80 >> bit):
                dst.append(data[src])
                src += 1
                continue
            b1, b2 = data[src], data[src + 1]
            src += 2
            dist = ((b1 & 0xF) << 8 | b2) + 1
            n = b1 >> 4
            if n == 0:
                n = data[src] + 0x12
                src += 1
            else:
                n += 2
            start = len(dst) - dist
            if dist >= n:
                dst += dst[start:start + n]
            else:
                for i in range(n):
                    dst.append(dst[start + i])
    return bytes(dst)


def u8_files(d):
    """{path: bytes} for every file in a U8 archive."""
    root = struct.unpack_from('>I', d, 4)[0]
    total = struct.unpack_from('>I', d, root + 8)[0]
    strings = root + total * 12
    files, stack = {}, []
    for i in range(total):
        e = root + i * 12
        name_off = struct.unpack_from('>I', d, e)[0] & 0xFFFFFF
        a, b = struct.unpack_from('>II', d, e + 4)
        name = d[strings + name_off:d.index(b'\0', strings + name_off)].decode('ascii')
        while stack and stack[-1][1] <= i:
            stack.pop()
        if d[e] == 1:
            stack.append((name, b))
        else:
            path = '/'.join([s[0] for s in stack[1:]] + [name])
            files[path.lstrip('./')] = d[a:a + b]
    return files


# --------------------------------------------------------------------------------------------
# BRRES
# --------------------------------------------------------------------------------------------

class Reader:
    def __init__(self, d):
        self.d = d

    def u8(self, o): return self.d[o]
    def u16(self, o): return struct.unpack_from('>H', self.d, o)[0]
    def s16(self, o): return struct.unpack_from('>h', self.d, o)[0]
    def u32(self, o): return struct.unpack_from('>I', self.d, o)[0]
    def s32(self, o): return struct.unpack_from('>i', self.d, o)[0]
    def f32(self, o): return struct.unpack_from('>f', self.d, o)[0]

    def string(self, o):
        return self.d[o:self.d.index(b'\0', o)].decode('ascii', 'replace')

    def dict(self, o):
        """Entries of a BRRES index group at o: [(name, absolute data offset)]."""
        n = self.u32(o + 4)
        out = []
        for i in range(1, n + 1):
            e = o + 8 + 16 * i
            out.append((self.string(o + self.s32(e + 8)), o + self.s32(e + 12)))
        return out


def brres_contents(r):
    """{folder name: [(name, offset)]} for a BRRES file."""
    assert r.d[:4] == b'bres', 'not a BRRES'
    root = r.u16(0xC)
    return {folder: r.dict(off) for folder, off in r.dict(root + 8)}



# --------------------------------------------------------------------------------------------
# MDL0 (version 11)
# --------------------------------------------------------------------------------------------

def mtx34(r, o):
    """Big-endian 3x4 matrix at o -> 4x4 numpy array."""
    m = np.eye(4)
    m[:3, :] = np.array(struct.unpack_from('>12f', r.d, o)).reshape(3, 4)
    return m


class Bone:
    pass


class Mdl0:
    # v11 section order
    DEFS, BONES, VERTS, NORMALS, COLORS, UVS, FURV, FURL, MATS, SHADERS, OBJS, TEXREFS, PALREFS, USER = range(14)

    def __init__(self, r, o, name):
        self.r, self.o, self.name = r, o, name
        ver = r.u32(o + 8)
        if ver not in (8, 11):
            raise ValueError('MDL0 version %d not supported' % ver)
        if ver == 8:
            # v8 (older models, e.g. the piranha plant pakkun_f): 11 sections, no fur and no user data; the rest of the header follows the section table directly
            raw = [r.s32(o + 0x10 + 4 * i) for i in range(11)]
            sec = raw[:6] + [0, 0] + raw[6:11] + [0]
            info = o + 0x10 + 11 * 4 + 4
        else:
            sec = [r.s32(o + 0x10 + 4 * i) for i in range(14)]
            info = o + 0x10 + 14 * 4 + 4
        self.version = ver
        self.sections = [r.dict(o + s) if s else [] for s in sec]
        wt = info + r.u32(info + 0x24)
        # matrix id -> bone index, or -1 for envelope (weighted) matrices
        self.matrix_bone = [r.s32(wt + 4 + 4 * i) for i in range(r.u32(wt))]

        self.bones = []
        for bname, bo in self.sections[self.BONES]:
            b = Bone()
            b.name, b.index, b.matrix_id = bname, r.u32(bo + 0xC), r.u32(bo + 0x10)
            b.flags = r.u32(bo + 0x14)
            b.scale = struct.unpack_from('>3f', r.d, bo + 0x20)
            b.rot = struct.unpack_from('>3f', r.d, bo + 0x2C)
            b.trans = struct.unpack_from('>3f', r.d, bo + 0x38)
            po = r.s32(bo + 0x5C)
            b.parent_off = bo + po if po else None
            b.offset = bo
            b.world = mtx34(r, bo + 0x70)
            b.inv_bind = mtx34(r, bo + 0xA0)
            self.bones.append(b)
        by_off = {b.offset: b for b in self.bones}
        for b in self.bones:
            b.parent = by_off.get(b.parent_off)
        self.bones.sort(key=lambda b: b.index)



# Vertex array component formats: u8, s8, u16, s16, f32
COMP_FMT = {0: ('B', 1), 1: ('b', 1), 2: ('H', 2), 3: ('h', 2), 4: ('f', 4)}


def read_array(r, o, ncomp_of):
    """Decode an MDL0 position/normal/uv array header at o -> float array (count, comps)."""
    data = o + r.s32(o + 8)
    fmt = r.u32(o + 0x18)
    divisor, stride, count = r.u8(o + 0x1C), r.u8(o + 0x1D), r.u16(o + 0x1E)
    ncomp = ncomp_of(r.u32(o + 0x14))
    ch, _ = COMP_FMT[fmt]
    vals = np.empty((count, ncomp))
    for i in range(count):
        vals[i] = struct.unpack_from('>%d%s' % (ncomp, ch), r.d, data + i * stride)
    if fmt != 4:
        vals /= float(1 << divisor)
    return vals


def read_colors(r, o):
    data = o + r.s32(o + 8)
    fmt, stride, count = r.u32(o + 0x18), r.u8(o + 0x1C), r.u16(o + 0x1E)
    out = np.ones((count, 4))
    for i in range(count):
        p = data + i * stride
        if fmt == 0:    # RGB565
            v = r.u16(p)
            out[i, :3] = ((v >> 11) / 31, (v >> 5 & 63) / 63, (v & 31) / 31)
        elif fmt in (1, 2):  # RGB8 / RGBX8
            out[i, :3] = [c / 255 for c in r.d[p:p + 3]]
        elif fmt == 3:  # RGBA4
            v = r.u16(p)
            out[i] = [(v >> s & 15) / 15 for s in (12, 8, 4, 0)]
        elif fmt == 4:  # RGBA6
            v = r.d[p] << 16 | r.d[p + 1] << 8 | r.d[p + 2]
            out[i] = [(v >> s & 63) / 63 for s in (18, 12, 6, 0)]
        elif fmt == 5:  # RGBA8
            out[i] = [c / 255 for c in r.d[p:p + 4]]
    return out


class Mesh:
    """One MDL0 object, triangulated, with per-vertex attributes (not yet in model space)."""


def parse_object(m, o):
    r = m.r
    mesh = Mesh()
    mesh.name = r.string(o + r.s32(o + 0x38))
    mesh.index = r.u32(o + 0x3C)
    single_mtx = r.s32(o + 8)
    vcd = r.u32(o + 0xC) | r.u32(o + 0x10) << 17  # CP VCD_LO (bits 0-16) + VCD_HI
    prim = o + 0x24 + r.s32(o + 0x2C)
    prim_size = r.u32(o + 0x28)
    ids = [r.s16(o + 0x48 + 2 * i) for i in range(12)]  # pos, nrm, clr0, clr1, uv0-7
    node_tab = o + r.s32(o + (0x60 if getattr(m, 'version', 11) == 8 else 0x64))
    slots = [r.u16(node_tab + 4 + 2 * i) for i in range(r.u32(node_tab))]

    # GX vertex attribute order: PNMTXIDX, TEX0-7MTXIDX, POS, NRM, CLR0, CLR1, TEX0-7
    attrs = [('pnmtx', vcd & 1)]
    attrs += [('texmtx%d' % i, vcd >> (1 + i) & 1) for i in range(8)]
    attrs += [('pos', vcd >> 9 & 3), ('nrm', vcd >> 11 & 3), ('clr0', vcd >> 13 & 3),
              ('clr1', vcd >> 15 & 3)]
    attrs += [('uv%d' % i, vcd >> (17 + 2 * i) & 3) for i in range(8)]
    attrs = [(n, t) for n, t in attrs if t]
    for name, t in attrs:
        if t == 1 and name != 'pnmtx' and not name.startswith('texmtx'):
            raise ValueError('direct vertex attribute %s not supported' % name)

    verts, tris = [], []
    slot_matrix = dict(enumerate(slots))  # matrix slot -> matrix id
    p, end = prim, prim + prim_size
    while p < end:
        op = r.u8(p)
        p += 1
        if op == 0:
            continue
        if op == 0x08:
            p += 5
            continue
        if op == 0x10:
            p += 4 + 4 * (r.u16(p) + 1)
            continue
        if op == 0x20:
            # Load a position matrix into a slot mid-draw (the GPU only has 10). Later
            # vertices' PNMTXIDX refer to whatever matrix the slot holds at that point.
            slot_matrix[(r.u16(p + 2) & 0xFFF) // 12] = r.u16(p)
            p += 4
            continue
        if op in (0x28, 0x30, 0x38):  # normal / texture / light matrix loads
            p += 4
            continue
        if op & 0x80 == 0:
            raise ValueError('unknown display list opcode %#x in %s' % (op, mesh.name))
        kind = op & 0xF8
        n = r.u16(p)
        p += 2
        first = len(verts)
        for _ in range(n):
            v = {}
            for name, t in attrs:
                if t == 3:
                    v[name] = r.u16(p)
                    p += 2
                else:
                    v[name] = r.u8(p)
                    p += 1
            if 'pnmtx' in v:
                v['mtx'] = slot_matrix.get(v['pnmtx'] // 3)
            verts.append(v)
        idx = range(first, first + n)
        if kind == 0x90:    # triangles
            tris += [(idx[i], idx[i + 1], idx[i + 2]) for i in range(0, n - 2, 3)]
        elif kind == 0x98:  # triangle strip
            for i in range(n - 2):
                a, b, c = idx[i], idx[i + 1], idx[i + 2]
                tris.append((a, b, c) if i % 2 == 0 else (b, a, c))
        elif kind == 0xA0:  # triangle fan
            tris += [(idx[0], idx[i], idx[i + 1]) for i in range(1, n - 1)]
        elif kind == 0x80:  # quads
            for i in range(0, n - 3, 4):
                tris += [(idx[i], idx[i + 1], idx[i + 2]), (idx[i], idx[i + 2], idx[i + 3])]
        # lines and points are ignored

    names = {n for n, _ in attrs}

    def gather(section, k, attr, reader):
        if ids[k] < 0 or attr not in names:
            return None
        arr = reader(m.sections[section][ids[k]][1])
        return np.array([arr[v[attr]] for v in verts])

    mesh.positions = gather(Mdl0.VERTS, 0, 'pos', lambda o: read_array(r, o, lambda f: 3 if f else 2))
    if mesh.positions is not None and mesh.positions.shape[1] == 2:   # flat quads store just X and Y
        mesh.positions = np.hstack([mesh.positions, np.zeros((len(mesh.positions), 1))])
    nrm = gather(Mdl0.NORMALS, 1, 'nrm', lambda o: read_array(r, o, lambda f: 3 if f == 0 else 9))
    mesh.normals = None if nrm is None else nrm[:, :3]
    mesh.colors = gather(Mdl0.COLORS, 2, 'clr0', lambda o: read_colors(r, o))
    mesh.uvs = gather(Mdl0.UVS, 4, 'uv0', lambda o: read_array(r, o, lambda f: 2 if f else 1))
    mesh.uv_sets = {0: mesh.uvs} if mesh.uvs is not None else {}
    for k in range(1, 8):  # tracks use a second UV set for baked shadow maps
        uvk = gather(Mdl0.UVS, 4 + k, 'uv%d' % k, lambda o: read_array(r, o, lambda f: 2 if f else 1))
        if uvk is not None:
            mesh.uv_sets[k] = uvk

    def matrix_id(v):
        if 'mtx' in v:
            return v['mtx']  # resolved from PNMTXIDX / 3 (a matrix slot) while parsing
        return single_mtx if single_mtx >= 0 else slots[0]

    mesh.matrix_ids = np.array([matrix_id(v) for v in verts], dtype=int)
    mesh.triangles = np.array(tris, dtype=np.uint32).reshape(-1, 3)
    return mesh


def model_space(m, mesh, bone_world=None):
    """Positions/normals of mesh in model space. Vertices bound to a single bone are stored in
    that bone's space; bone_world (default: bind pose) maps bone index -> 4x4 world matrix."""
    world = bone_world or {b.index: b.world for b in m.bones}
    pos = np.array(mesh.positions, dtype=float)
    nrm = None if mesh.normals is None else np.array(mesh.normals, dtype=float)
    for mid in np.unique(mesh.matrix_ids):
        bone = m.matrix_bone[mid] if mid < len(m.matrix_bone) else -1
        if bone < 0:
            continue  # envelope matrix: vertices are already in model space
        sel = mesh.matrix_ids == mid
        w = world[bone]
        pos[sel] = pos[sel] @ w[:3, :3].T + w[:3, 3]
        if nrm is not None:
            nrm[sel] = nrm[sel] @ w[:3, :3].T
    if nrm is not None:
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
    return pos, nrm



# --------------------------------------------------------------------------------------------
# Skeleton posing (CHR0 bone animations)
# --------------------------------------------------------------------------------------------

def srt_matrix(scale, rot_deg, trans):
    """NW4R bone local matrix: T * Rz * Ry * Rx * S."""
    rx, ry, rz = (math.radians(a) for a in rot_deg)
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    m = np.eye(4)
    m[:3, :3] = Rz @ Ry @ Rx @ np.diag(scale)
    m[:3, 3] = trans
    return m


def pose_world(m, local_srt):
    """Bone world matrices from {bone index: (scale, rot, trans)}, walking parents.

    Driver skeletons use segment scale compensation (bone flags 0x20/0x40, as in Maya): a bone's
    scale stretches its own segment and moves its children (their translation is in the scaled
    parent space) but is not inherited by their rotation/scale. Animations rely on this to
    lengthen arms without stretching the hands."""
    frame, world = {}, {}  # frame: position + rotation only (what children hang from)

    def get(b):
        if b.index not in frame:
            scale, rot, trans = local_srt[b.index]
            if b.parent is None:
                t = np.array(trans, dtype=float)
                parent = np.eye(4)
            else:
                get(b.parent)
                t = np.array(local_srt[b.parent.index][0], dtype=float) * trans
                parent = frame[b.parent.index]
            frame[b.index] = parent @ srt_matrix((1, 1, 1), rot, t)
            world[b.index] = frame[b.index] @ np.diag([*scale, 1.0])
        return world[b.index]

    for b in m.bones:
        get(b)
    return world


def chr0_frame_count(r, o):
    return r.u16(o + (0x1C if r.u32(o + 8) == 3 else 0x20))   # v3 (older files) has no user-data pointer: the header fields sit one word earlier


def chr0_pose(r, o, frame):
    """{bone name: {'scale'|'rot'|'trans': [x, y, z]}} sampled at `frame` (may be fractional)."""
    ver = r.u32(o + 8)
    if ver not in (3, 4, 5):
        raise ValueError('CHR0 version %d not supported' % ver)
    out = {}
    for name, e in r.dict(o + r.s32(o + 0x10)):
        code = r.u32(e + 4)
        p = e + 8
        res = {}
        for kind, has_bit, iso_bit, fixed_bit, fmt_shift, fmt_mask in (
                ('scale', 22, 4, 13, 25, 3), ('rot', 23, 5, 16, 27, 7), ('trans', 24, 6, 19, 30, 3)):
            if ver == 3:      # older files: the REQUIRE_* bits are not set; a channel exists unless its NOT_EXIST bits (identity / one / zero / use-model) say so
                if code & {'scale': 0x8A, 'rot': 0x126, 'trans': 0x246}[kind]:
                    continue
            elif not code >> has_bit & 1:
                continue
            fmt = code >> fmt_shift & fmt_mask
            n = 1 if (code >> iso_bit & 1 and (kind == 'scale' or ver != 3)) else 3
            vals = []
            for c in range(n):
                if fmt == 0 or code >> (fixed_bit + c) & 1:    # (format 0 = constant: the value itself sits in the slot)
                    vals.append(r.f32(p))
                else:
                    vals.append(anim_value(r, e + r.s32(p), fmt, kind == 'rot', frame))
                p += 4
            res[kind] = vals * 3 if n == 1 else vals
        out[name] = res
    return out


def _hermite(keys, frame):
    """keys: [(frame, value, slope)] sorted by frame."""
    if frame <= keys[0][0]:
        return keys[0][1]
    for (f0, v0, t0), (f1, v1, t1) in zip(keys, keys[1:]):
        if frame <= f1:
            span = f1 - f0
            if span <= 0:
                return v1
            t = (frame - f0) / span
            t2, t3 = t * t, t * t * t
            return ((2 * t3 - 3 * t2 + 1) * v0 + (t3 - 2 * t2 + t) * span * t0 +
                    (-2 * t3 + 3 * t2) * v1 + (t3 - t2) * span * t1)
    return keys[-1][1]


def anim_value(r, o, fmt, is_rot, frame):
    """Sample an animation curve: formats 1-3 are hermite keyframes (4/6/12-byte keys);
    rotation formats 4-6 are one value per frame (8-bit, 16-bit, float)."""
    if fmt in (1, 2):
        count, step, base = r.u16(o), r.f32(o + 8), r.f32(o + 12)
        keys = []
        for i in range(count):
            if fmt == 1:
                d = r.u32(o + 16 + 4 * i)
                tangent = ((d & 0xFFF) ^ 0x800) - 0x800
                keys.append((d >> 24, base + step * (d >> 12 & 0xFFF), tangent / 32))
            else:
                k = o + 16 + 6 * i
                keys.append((r.u16(k) / 32, base + step * r.u16(k + 2), r.s16(k + 4) / 256))
        return _hermite(keys, frame)
    if fmt == 3:
        count = r.u16(o)
        keys = [struct.unpack_from('>3f', r.d, o + 8 + 12 * i) for i in range(count)]
        return _hermite(keys, frame)
    if fmt in (4, 5, 6):
        # linear tracks: one value per frame. 8/16-bit ones start with a float scale and bias (value = raw * scale + bias); they are angles, interpolated periodically
        # (a plain lerp between 355 and 5 degrees swings through 180: the penguins' flipped-over walk)
        i0 = int(math.floor(frame))
        i1, t = i0 + 1, frame - i0
        if fmt == 6:
            read = lambda i: r.f32(o + 4 * i)
        else:
            scale, bias = r.f32(o), r.f32(o + 4)
            read = (lambda i: r.u8(o + 8 + i) * scale + bias) if fmt == 4 else (lambda i: r.u16(o + 8 + 2 * i) * scale + bias)
        a = read(i0)
        if not t:
            return a
        bb = read(i1)
        d = (bb - a + 180.0) % 360.0 - 180.0
        return a + d * t
    raise ValueError('animation format %d not supported' % fmt)


def posed_world(m, anim):
    """Bone world matrices for a CHR0 frame-0 pose; unanimated channels keep the bind value."""
    local = {}
    for b in m.bones:
        a = anim.get(b.name, {})
        local[b.index] = (a.get('scale', b.scale), a.get('rot', b.rot), a.get('trans', b.trans))
    return pose_world(m, local)



# --------------------------------------------------------------------------------------------
# Textures (TEX0) and materials
# --------------------------------------------------------------------------------------------

# GX texture formats: id -> (block width, block height, bits per pixel)
TEX_BLOCKS = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16),
              5: (4, 4, 16), 6: (4, 4, 32), 14: (8, 8, 4)}


def _unblock(blocks, w, h, bw, bh, pw, ph):
    """(nblocks, bh, bw, C) in GX block order -> (ph, pw, C) image, cropped to (h, w)."""
    by, bx = ph // bh, pw // bw
    img = blocks.reshape(by, bx, bh, bw, -1).transpose(0, 2, 1, 3, 4).reshape(ph, pw, -1)
    return img[:h, :w]


def _rgb565(v):
    r = (v >> 11 & 31) * 255 // 31
    g = (v >> 5 & 63) * 255 // 63
    b = (v & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.uint8)


def decode_texture(r, o):
    """TEX0 at o -> PIL RGBA image (top mip level)."""
    data = o + r.s32(o + 0x10)
    w, h, fmt = r.u16(o + 0x1C), r.u16(o + 0x1E), r.u32(o + 0x20)
    if fmt not in TEX_BLOCKS:
        raise ValueError('texture format %d not supported' % fmt)
    bw, bh, bpp = TEX_BLOCKS[fmt]
    pw, ph = -(-w // bw) * bw, -(-h // bh) * bh
    size = pw * ph * bpp // 8
    raw = np.frombuffer(r.d, np.uint8, size, data)
    nblocks = (pw // bw) * (ph // bh)

    if fmt == 0:    # I4
        v = np.stack([raw >> 4, raw & 15], -1).reshape(nblocks, bh, bw) * 17
        rgba = np.stack([v, v, v, np.full_like(v, 255)], -1)
    elif fmt == 1:  # I8
        v = raw.reshape(nblocks, bh, bw)
        rgba = np.stack([v, v, v, np.full_like(v, 255)], -1)
    elif fmt == 2:  # IA4
        v = raw.reshape(nblocks, bh, bw)
        i, a = (v & 15) * 17, (v >> 4) * 17
        rgba = np.stack([i, i, i, a], -1)
    elif fmt == 3:  # IA8
        v = raw.reshape(nblocks, bh, bw, 2)
        rgba = np.stack([v[..., 1], v[..., 1], v[..., 1], v[..., 0]], -1)
    elif fmt == 4:  # RGB565
        v = raw.view('>u2').astype(np.uint32).reshape(nblocks, bh, bw)
        rgba = np.concatenate([_rgb565(v), np.full(v.shape + (1,), 255, np.uint8)], -1)
    elif fmt == 5:  # RGB5A3
        v = raw.view('>u2').astype(np.uint32).reshape(nblocks, bh, bw)
        opaque = v >> 15 == 1
        c5 = lambda s: (v >> s & 31) * 255 // 31
        c4 = lambda s: (v >> s & 15) * 17
        rgba = np.stack([np.where(opaque, c5(10), c4(8)), np.where(opaque, c5(5), c4(4)),
                         np.where(opaque, c5(0), c4(0)), np.where(opaque, 255, (v >> 12 & 7) * 255 // 7)],
                        -1).astype(np.uint8)
    elif fmt == 6:  # RGBA8: each block is 32 bytes of AR then 32 bytes of GB
        v = raw.reshape(nblocks, 2, 16, 2)
        rgba = np.stack([v[:, 0, :, 1], v[:, 1, :, 0], v[:, 1, :, 1], v[:, 0, :, 0]], -1)
        rgba = rgba.reshape(nblocks, 4, 4, 4)
    else:           # CMPR: 8x8 blocks of four 4x4 DXT1 sub-blocks, big-endian
        sub = raw.reshape(nblocks, 4, 8)
        c0 = sub[..., 0].astype(np.uint32) << 8 | sub[..., 1]
        c1 = sub[..., 2].astype(np.uint32) << 8 | sub[..., 3]
        p0, p1 = _rgb565(c0).astype(np.int32), _rgb565(c1).astype(np.int32)
        four = (c0 > c1)[..., None]
        p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
        p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
        a3 = np.where(c0 > c1, 255, 0)
        pal = np.stack([p0, p1, p2, p3], -2)                              # (n, 4, 4 colours, 3)
        alpha = np.stack([np.full_like(a3, 255)] * 3 + [a3], -1)          # (n, 4, 4)
        bits = sub[..., 4:8]                                              # one byte per row
        idx = np.stack([bits >> s & 3 for s in (6, 4, 2, 0)], -1)         # (n, 4, 4 rows, 4 cols)
        n = np.arange(nblocks)[:, None, None, None]
        s = np.arange(4)[None, :, None, None]
        rgb = pal[n, s, idx]                                              # (n, 4, 4, 4, 3)
        a = alpha[n, s, idx]
        px = np.concatenate([rgb, a[..., None]], -1).astype(np.uint8)     # (n, sub, row, col, 4)
        # arrange the 2x2 sub-blocks into 8x8
        px = px.reshape(nblocks, 2, 2, 4, 4, 4).transpose(0, 1, 3, 2, 4, 5).reshape(nblocks, 8, 8, 4)
        rgba = px
    img = _unblock(np.asarray(rgba, np.uint8), w, h, bw, bh, pw, ph)
    out = Image.fromarray(img, 'RGBA')
    out.info['gxfmt'] = fmt      # kept so materials can treat intensity textures (I4/I8) specially
    out.info['mips'] = r.u32(o + 0x24)    # number of mip levels the game stores (GX never samples below the last one)
    return out


# GX wrap modes -> glTF sampler wrap
WRAP = {0: 33071, 1: 10497, 2: 33648}  # clamp, repeat, mirror


class Material:
    pass


def parse_materials(m):
    """{material index: Material} with texture refs, cull mode and translucency."""
    r = m.r
    out = {}
    for name, o in m.sections[Mdl0.MATS]:
        mat = Material()
        mat.name, mat.index = name, r.u32(o + 0xC)
        mat.off = o
        mat.xlu = bool(r.u32(o + 0x10) & 0x80000000)
        mat.cull = r.u32(o + 0x18)  # 0 none, 1 front, 2 back, 3 all
        # Texture SRT of the base texture (scale S/T, rotation, translation S/T). The game only
        # uses scale here, e.g. (2, 1) on eye textures: half a face, mirrored by the wrap mode.
        so = 0x1AC if getattr(m, 'version', 11) == 8 else 0x1B0     # v8 has no fur pointer in the header: everything after it sits 4 bytes earlier
        mat.uv_scale = np.array(struct.unpack_from('>2f', r.d, o + so))
        # Every texture layer has its own SRT (scale S/T, rotation, translation S/T), 0x14 bytes apart
        mat.layer_srt = [list(struct.unpack_from('>5f', r.d, o + so + 0x14 * k)) for k in range(min(8, r.u32(o + 0x2C)))]
        mat.textures = []
        refs = o + r.s32(o + 0x30)
        for i in range(r.u32(o + 0x2C)):
            t = refs + 0x34 * i
            mat.textures.append({'name': r.string(t + r.s32(t)),
                                 'wrap': (r.u32(t + 0x18), r.u32(t + 0x1C))})
        try:
            mat.regular = material_layers(r, o)
        except Exception:
            mat.regular = []
        try:
            mat.blend = material_blend(r, o)
        except Exception:
            mat.blend = None
        try:
            mat.vtx_mix = material_vtx_mix(r, o)
        except Exception:
            mat.vtx_mix = None
        try:
            mat.alpha_blend = material_alpha_blend(r, o)
        except Exception:
            mat.alpha_blend = False
        try:
            mat.shadow_color, mat.shadow_mode = material_shadow_color(r, o)
        except Exception:
            mat.shadow_color, mat.shadow_mode = None, None
        try:
            mat.tev_scale = material_tev_scale(r, o)
            # a lighting ramp ('lm_0') means the x2 stages double a LIT colour (lerp(C2, tex, light) + ...), not tex x vertex colour: the page has
            # no light term, so applying the x2 washes the picture out (the Thwomp)
            if any(re.match(r'lm_\d', t['name']) for t in mat.textures):
                mat.tev_scale = 1
        except Exception:
            mat.tev_scale = 1
        try:
            mat.alpha_compare = material_alpha_compare(r, o)
        except Exception:
            mat.alpha_compare = None
        out[mat.index] = mat
    return out


def _bp_writes(d):
    """(register, 24-bit value) of every GX BP write (opcode 0x61) in a display-list blob."""
    out, i = [], 0
    while i < len(d) - 4:
        if d[i] == 0x61:
            out.append((d[i + 1], int.from_bytes(d[i + 2:i + 5], 'big')))
            i += 5
        else:
            i += 1
    return out


def material_layers(r, mat_off):
    """Texture layers that are sampled with a plain UV set, in TEV stage order:
    [(layer index, uv set)]. Layers using generated coordinates (environment/normal maps) are
    left out. Layer k of a material is the k-th texture reference; which UV set feeds it comes
    from the shader's TEV stage order (BP regs 0x28-0x2F, texmap -> texcoord) and the material's
    XF texgens (texcoord -> source row 5-12 = UV set 0-7)."""
    sh = mat_off + r.s32(mat_off + 0x28)
    sd = r.d[sh:sh + r.u32(sh)]
    texmap_layer = list(sd[0x10:0x18])
    tref = {reg: v for reg, v in _bp_writes(sd) if 0x28 <= reg <= 0x2F}

    md = r.d[mat_off:mat_off + r.u32(mat_off)]
    texgen = {}
    for i in range(len(md) - 9):
        if md[i] == 0x10 and md[i + 3] == 0x10 and 0x40 <= md[i + 4] <= 0x47:
            v = struct.unpack_from('>I', md, i + 5)[0]
            texgen[md[i + 4] - 0x40] = (v >> 7 & 31, v >> 4 & 7)

    out = []
    for reg in sorted(tref):
        for shift in (0, 12):
            v = tref[reg]
            if not v >> (shift + 6) & 1:
                continue
            layer = texmap_layer[v >> shift & 7]
            row, typ = texgen.get(v >> (shift + 3) & 7, (None, None))
            if layer != 255 and typ == 0 and row is not None and 5 <= row <= 12:
                if (layer, row - 5) not in out:
                    out.append((layer, row - 5))
    return out


def choose_layers(mat, textures):
    """Track materials: (base, shadow) as ((layer, uv set), (layer, uv set) or None). The shadow
    layer is the baked shadow map, recognisable in every course by 'kage' (Japanese for shadow) or
    'shadow' in its name; the game multiplies it over the diffuse texture. It can sit on UV set 0
    or 1 depending on the course, so the UV sets come from the material, not from convention."""
    def is_shadow(layer):
        return re.search(r'kage|shadow', mat.textures[layer]['name'], re.I) is not None

    regular = [(l, uv) for l, uv in getattr(mat, 'regular', []) if l < len(mat.textures)]
    if not regular:
        return (0, 0), None
    if getattr(mat, 'shadow_color', None) and len(regular) >= 2:
        # TEV stage 0 blends towards the shadow colour by its texture, so that texture is the shadow map even when
        # its name doesn't say so (GCN Mario Circuit calls it 'ma_ka03')
        return regular[1], regular[0]
    # 'lm_0'-style textures are lighting ramps (the game's light-map shading, e.g. the Thwomp's second layer), never the picture itself
    ramp = lambda layer: re.match(r'lm_\d', mat.textures[layer]['name']) is not None
    bases = [(l, uv) for l, uv in regular if not is_shadow(l) and not ramp(l)] or [(l, uv) for l, uv in regular if not ramp(l)][:1] or regular[:1]
    shadows = [(l, uv) for l, uv in regular if is_shadow(l) and (l, uv) != bases[0]]
    return bases[0], (shadows[0] if shadows else None)


def sky_is_screen(sd):
    """True when the last TEV colour stage is lerp(CPREV, ONE, TEXC) (a=CPREV b=ONE c=TEXC d=ZERO): the layer is screened over the previous result."""
    cols = [v for g, v in _bp_writes(sd) if g in (0xC0, 0xC2, 0xC4, 0xC6)]
    return len(cols) >= 2 and (cols[-1] & 0xFFFF) == 0x0C8F


def choose_sky_layers(r, mat_off, mat):
    """Sky materials (vrcorn_model.brres) with two texture layers, neither a baked shadow map (e.g.
    water_course's WT_VRcorn: a complete sky+cloud image plus a second, independently-scrolling cloud
    accent layer; rainbow_course's vr_starsky: a galaxy backdrop plus a twinkling-star accent layer).
    choose_layers()'s single-base heuristic silently drops whichever of these two isn't picked as
    'base' -- for water_course that meant the real colour sky texture (WT_sky01) never showed at all,
    just its accent layer (WT_cloud01, a mostly-black cloud silhouette texture) alone, rendering as a
    harsh black sky with white cloud blobs instead of a proper blue sky. Both layers are meant to be
    ADDED together. Returns ((dominant_layer, uv), (accent_layer, uv) or None): the layer whose TEV
    colour stage runs LAST is the dominant/base one (its own stage typically close to full strength);
    any earlier-stage layer is the accent, consistently given an explicit 0.5 TEV scale in every
    sample checked (water_course, old_peach_gc, koopa_course, rainbow_course)."""
    regular = [(l, uv) for l, uv in getattr(mat, 'regular', []) if l < len(mat.textures)]
    if len(regular) < 2:
        return (regular[0] if regular else (0, 0)), None
    sh = mat_off + r.s32(mat_off + 0x28)
    sd = r.d[sh:sh + r.u32(sh)]
    texmap_layer = list(sd[0x10:0x18])
    stage_layer = {}
    for reg, v in _bp_writes(sd):
        if 0x28 <= reg <= 0x2F:
            base_stage = (reg - 0x28) * 2
            for shift, stage in ((0, base_stage), (12, base_stage + 1)):
                slot = v >> shift & 7
                layer = texmap_layer[slot] if slot < len(texmap_layer) else 255
                if layer < len(mat.textures):
                    stage_layer[stage] = layer
    order = sorted(stage_layer)
    if len(order) < 2:
        return regular[0], None
    dominant_layer, accent_layer = stage_layer[order[-1]], stage_layer[order[0]]
    if sky_is_screen(sd):    # GCN Peach Beach: the FIRST stage's layer is the sky, the second blends the cloud bank over it (screen), not the other way round
        dominant_layer, accent_layer = accent_layer, dominant_layer
    dominant = next(((l, uv) for l, uv in regular if l == dominant_layer), regular[0])
    accent = next(((l, uv) for l, uv in regular if l == accent_layer and l != dominant[0]), None)
    return dominant, accent


def material_tev_scale(r, mat_off):
    """Brightness factor of the material's colour TEV network (BP 0xC0, 0xC2, ...; scale bits
    20-21: 0 = x1, 1 = x2, 2 = x4, 3 = x1/2). Track and object materials multiply texture x vertex
    colour and then double it, so without this the whole course comes out half as bright.
    Takes the max scale across all colour stages rather than just the last one: a material can have
    a trailing stage (e.g. a decal/detail blend on top) whose own scale is x1, which doesn't undo an
    earlier stage's x2 -- e.g. Daisy Circuit's road_m is shadow-lerp(x1) -> texture*vertexColor(x2) ->
    detail blend(x1); reading only the last stage silently dropped the x2 every other course material
    gets, leaving the entire drivable road rendered at half brightness."""
    sh = mat_off + r.s32(mat_off + 0x28)
    scale = 1
    for reg, v in _bp_writes(r.d[sh:sh + r.u32(sh)]):
        if 0xC0 <= reg <= 0xDE and reg % 2 == 0:
            scale = max(scale, (1, 2, 4, 0.5)[v >> 20 & 3])
    return scale


def _tev_color_reg(r, mat_off, lo_addr, konst):
    """[r, g, b, a] 0-1 from a TEV_REGISTERL/H pair (BP lo_addr/lo_addr+1): low 11 bits of lo are R,
    bits 12-22 of lo are A, low 11 bits of hi are B, bits 12-22 of hi are G. `konst` selects which
    write to trust -- GX BP writes to this same address pair serve two different roles (bit 23 clear
    = the plain colour register C0-C2 stage inputs reference directly; bit 23 set = the konst colour
    bank K0-K3 that KCSEL indirects through) -- materials that use both roles write each address
    twice, so reading the wrong bit23 silently picks up the other role's value."""
    lo = hi = None
    for reg, w in _bp_writes(r.d[mat_off:mat_off + r.u32(mat_off)]):
        if bool(w >> 23 & 1) == konst:
            if reg == lo_addr:
                lo = w
            elif reg == lo_addr + 1:
                hi = w
    if lo is None or hi is None:
        return None
    return [round((lo & 0x7FF) / 255, 4), round((hi >> 12 & 0x7FF) / 255, 4),
            round((hi & 0x7FF) / 255, 4), round((lo >> 12 & 0x7FF) / 255, 4)]


def _resolve_kcsel(r, mat_off, stage):
    """[r, g, b] 0-1 colour actually selected by a TEV stage's konst colour input (TEV_KSEL, BP
    0xF6 + stage, one register per stage) -- lives in the material's TEV stage display list (same
    sub-block as the colour/alpha combiner stages at 0xC0+), not its main display list. Simulates
    GX's masked-BP-write convention (a preceding write to BP 0xFE limits which bits of the following
    write actually apply, leaving the rest at whatever an earlier write to the same address left
    them) to reconstruct the register's true final value: compilers often set the swap-table bits
    (0-3) and the kcsel/kasel bits (4-13) in two separate masked writes rather than one, so reading
    either write alone misses half the picture."""
    sh = mat_off + r.s32(mat_off + 0x28)
    reg_addr = 0xF6 + stage
    reg_val, mask = 0, 0xFFFFFF
    for reg, v in _bp_writes(r.d[sh:sh + r.u32(sh)]):
        if reg == 0xFE:
            mask = v
        elif reg == reg_addr:
            reg_val = (reg_val & ~mask) | (v & mask)
            mask = 0xFFFFFF
    kcsel = reg_val >> 4 & 0x1F
    fixed = (1.0, 7 / 8, 3 / 4, 5 / 8, 1 / 2, 3 / 8, 1 / 4, 1 / 8)
    if kcsel < len(fixed):
        return [round(fixed[kcsel], 4)] * 3
    if kcsel > 0x1F:
        return None
    k, channel = (kcsel - 0x0C) % 4, (kcsel - 0x0C) // 4   # K0-3, then 0=full RGB, 1/2/3/4=R/G/B/A
    rgba = _tev_color_reg(r, mat_off, 0xE0 + 2 * k, konst=True)
    if rgba is None:
        return None
    return rgba[:3] if channel == 0 else [rgba[channel - 1]] * 3


def _resolve_kasel(r, mat_off, stage=0):
    """Alpha (0-1) selected by a TEV stage's konst ALPHA input (TEV_KSEL bits 9-13 for even stages,
    19-23 for odd ones), with the same masked-write reconstruction as _resolve_kcsel."""
    sh = mat_off + r.s32(mat_off + 0x28)
    reg_addr = 0xF6 + stage // 2
    reg_val, mask = 0, 0xFFFFFF
    for reg, v in _bp_writes(r.d[sh:sh + r.u32(sh)]):
        if reg == 0xFE:
            mask = v
        elif reg == reg_addr:
            reg_val = (reg_val & ~mask) | (v & mask)
            mask = 0xFFFFFF
    kasel = reg_val >> (9 if stage % 2 == 0 else 19) & 0x1F
    fixed = (1.0, 7 / 8, 3 / 4, 5 / 8, 1 / 2, 3 / 8, 1 / 4, 1 / 8)
    if kasel < len(fixed):
        return fixed[kasel]
    if kasel < 0x10:
        return None
    k, channel = (kasel - 0x10) % 4, (kasel - 0x10) // 4   # K0-3, then 0=R 1=G 2=B 3=A
    rgba = _tev_color_reg(r, mat_off, 0xE0 + 2 * k, konst=True)
    return None if rgba is None else rgba[channel]


def material_shadow_color(r, mat_off):
    """Course materials with a baked shadow map tint shadowed areas toward a constant colour (often
    a lavender ambient tone, occasionally black) instead of just multiplying the vertex colour
    straight through. Three different TEV stage-0 shapes do this across the game's courses, all
    using the same shadow texture and the same underlying idea, just arranged (and konst-sourced)
    differently by whichever artist/compiler pass built each material:
      'lerp':     lerp(konst, vertex colour, shadow texture) -- lit areas show the vertex colour,
                  fully-shadowed ones show pure konst (Daisy Circuit's road_m: konst via the C0-C2
                  stage-input slots; the DS/GBA/N64/SNES retro ports' road/terrain materials: konst
                  via KCSEL indirection instead, which happens to resolve to black for every one of
                  them checked, making this mathematically identical to a plain shadow multiply --
                  but reading it as the 'additive' shape below instead, before KCSEL was decoded,
                  would have sent non-black vertex colours to black whenever shadow sampled low).
      'additive': (vertex colour * shadow texture) + konst -- the konst is a floor added back in
                  after modulating by vertex colour, not a target lerped towards (Daisy Circuit's
                  building walls, con00/01/02, stonePavement_m, grassland_m). Reading this stage as a
                  plain multiply (as if mode were irrelevant) sends any vertex colour of pure black
                  straight to pure black instead of the intended konst-tinted result -- that silently
                  blacked out whole building walls where baked vertex colour happened to hit zero.
    Returns (colour [r, g, b] 0-1, mode) or (None, None) for materials that don't work either way."""
    sh = mat_off + r.s32(mat_off + 0x28)
    stage0 = [v for reg, v in _bp_writes(r.d[sh:sh + r.u32(sh)]) if reg == 0xC0]
    if not stage0:
        return None, None
    v = stage0[0]
    a, b_, c, d = v >> 12 & 15, v >> 8 & 15, v >> 4 & 15, v & 15
    if a in (2, 4, 6) and b_ == 8 and c == 10 and d == 15:   # lerp(C0-2, TEXC, RASC): the vertex colour is the blend factor (see material_vtx_mix)
        return None, None
    if a in (2, 4, 6) and b_ == 10 and c == 8:           # lerp(konst C0-2, RASC, TEXC)
        rgba = _tev_color_reg(r, mat_off, 0xE0 + 2 * (a // 2), konst=False)
        return (rgba[:3], 'lerp') if rgba is not None else (None, None)
    if a == 14 and b_ == 10 and c == 8:                  # lerp(konst via KCSEL, RASC, TEXC)
        color = _resolve_kcsel(r, mat_off, 0)
        return (color, 'lerp') if color is not None else (None, None)
    if a == 15 and b_ == 8 and c == 10 and d in (2, 4, 6):   # RASC*TEXC + konst C0-2
        rgba = _tev_color_reg(r, mat_off, 0xE0 + 2 * (d // 2), konst=False)
        return (rgba[:3], 'additive') if rgba is not None else (None, None)
    return None, None


def material_vtx_mix(r, mat_off):
    """Single-stage materials whose colour is lerp(C0-2, TEXC, RASC) (a=C, b=TEXC, c=RASC, d=ZERO): the VERTEX colour picks between a constant and the texture
    (GBA Bowser Castle 3's walls and lava glow towards orange). Returns [r, g, b] 0-1 or None."""
    sh = mat_off + r.s32(mat_off + 0x28)
    stage0 = [v for reg, v in _bp_writes(r.d[sh:sh + r.u32(sh)]) if reg == 0xC0]
    if not stage0:
        return None
    v = stage0[0]
    a, b_, c, d = v >> 12 & 15, v >> 8 & 15, v >> 4 & 15, v & 15
    if a in (2, 4, 6) and b_ == 8 and c == 10 and d == 15:
        rgba = _tev_color_reg(r, mat_off, 0xE0 + 2 * (a // 2), konst=False)
        return rgba[:3] if rgba is not None else None
    return None


def material_blend(r, mat_off):
    """'add' if the material is drawn additively (glows, sun, lens flares), else None. Read from the
    GX blend-mode register (BP 0x41) in the material's display list: bit 0 enables blending,
    bits 5-7 are the destination factor (1 = one), bit 11 selects subtract."""
    md = r.d[mat_off:mat_off + r.u32(mat_off)]
    for reg, v in _bp_writes(md):
        if reg == 0x41:
            if v & 1 and not v >> 11 & 1 and (v >> 5 & 7) == 1:
                return 'add'
    return None


def material_alpha_blend(r, mat_off):
    """True when the material's GX blend mode (BP 0x41) is the ordinary 'src alpha / 1 - src alpha' blend (enable bit set, src factor 4, dst
    factor 5), even though the file doesn't flag the material translucent: GX still blends it, so its texture's transparent parts are
    see-through (the Bowser emblem on Bowser's Castle's floor, k_yazirusi01, drew as a black rectangle without this)."""
    md = r.d[mat_off:mat_off + r.u32(mat_off)]
    for reg, v in _bp_writes(md):
        if reg == 0x41:
            return bool(v & 1) and (v >> 8 & 7) == 4 and (v >> 5 & 7) == 5 and not v >> 11 & 1
    return False


def material_alpha_compare(r, mat_off):
    """Alpha-test threshold (0-1) from the GX alpha-compare register (BP 0xF3: ref0, ref1, comp0,
    comp1, 2-bit AND/OR/XOR/XNOR of the two), or None if the material never actually discards a
    pixel. A texture can carry an alpha channel for reasons that have nothing to do with cutout
    (a lighting mask, a glow mask, junk left in an unused channel); whether GX treats it as cutout
    is this register, not whether the channel happens to dip below 255 anywhere -- that blanket
    "any alpha < 255 -> treat as a 0.5 mask" guess was punching real holes in textures like the
    DK Summit cannon gate's lattice, whose alpha isn't a cutout mask at all.
    comp values: 0 NEVER, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NEQUAL, 6 GEQUAL, 7 ALWAYS.
    Approximates the common GEQUAL/GREATER "keep if alpha >= ref" shape (fences, leaves, foliage)
    as a straight alphaTest threshold; ALWAYS/ALWAYS (the two conditions never discard, whatever
    the 2-bit op combining them) returns None."""
    md = r.d[mat_off:mat_off + r.u32(mat_off)]
    for reg, v in _bp_writes(md):
        if reg == 0xF3:
            ref0 = v & 0xFF
            comp0 = v >> 16 & 7
            comp1 = v >> 19 & 7
            if comp0 == 7 and comp1 == 7:   # both ALWAYS: the test never discards anything
                return None
            if comp0 in (3, 4, 6):          # LEQUAL / GREATER / GEQUAL: the shape alphaTest can express
                return ref0 / 255
            return None                     # NEVER/LESS/EQUAL/NEQUAL-style tests don't map to alphaTest; skip rather than guess
    return None


def _srt0_track(r, p):
    """Keyframe list [[frame, value, tangent], ...] of a texture animation channel at p."""
    n = r.u16(p)
    return [[round(r.f32(p + 8 + 12 * i), 5), round(r.f32(p + 12 + 12 * i), 5), round(r.f32(p + 16 + 12 * i), 5)]
            for i in range(n)]


def parse_srt0(r, contents):
    """Texture SRT animations (AnmTexSrt) of a BRRES: {material name: {texture layer: anim}}, where
    anim = {'frames': n, 'scale': [su, sv], 'rot': r, 'trans': [tu, tv]} and each channel is None
    (not animated: keep the material's own value), a fixed number, or keyframes [[frame, value,
    tangent], ...]. Flags of each texture entry: 0x1 enabled; 0x2/0x4/0x8 = no scale/rotation/
    translation data; 0x10 scale is the same in U and V; 0x20/0x40/0x80/0x100/0x200 = scaleU,
    scaleV, rotation, transU, transV are fixed values (otherwise an offset to keyframes, relative
    to the address of the offset word itself). Verified against the game's course files."""
    out = {}
    for _name, o in contents.get('AnmTexSrt(NW4R)', []):
        frames = r.u16(o + 0x20)
        for mname, mo in r.dict(o + r.s32(o + 0x10)):
            enabled = r.u32(mo + 4)
            layers = [i for i in range(8) if enabled >> i & 1]
            for k, layer in enumerate(layers):
                e = mo + r.u32(mo + 0x0C + 4 * k)
                flags, p = r.u32(e), e + 4

                def take(fixed):
                    nonlocal p
                    word = r.u32(p)
                    val = r.f32(p) if fixed else _srt0_track(r, p + word)   # offsets are relative to the word itself
                    p += 4
                    return round(val, 5) if fixed else val

                su = sv = rot = tu = tv = None
                if not flags & 0x2:
                    su = take(flags & 0x20)
                    sv = su if flags & 0x10 else take(flags & 0x40)
                if not flags & 0x4:
                    rot = take(flags & 0x80)
                if not flags & 0x8:
                    tu = take(flags & 0x100)
                    tv = take(flags & 0x200)
                out.setdefault(mname, {})[layer] = {'frames': frames, 'scale': [su, sv], 'rot': rot, 'trans': [tu, tv]}
    out['PAT0_ANIMS'] = parse_pat0(r, contents)      # texture pattern animations travel with the scroll animations
    return out


def node_mix(m):
    """Vertex envelopes of a model: {envelope matrix id: [(bone index, weight), ...]}. The 'NodeMix'
    definitions hold two kinds of entries: op 5 maps a matrix id to a single bone (already in
    Mdl0.matrix_bone) and op 3 defines a weighted envelope over other matrix ids."""
    r, out = m.r, {}
    for name, o in m.sections[Mdl0.DEFS]:
        if name != 'NodeMix':
            continue
        p = o
        while r.u8(p) != 1:
            op = r.u8(p)
            if op == 3:
                mid, cnt = r.u16(p + 1), r.u8(p + 3)
                out[mid] = [(m.matrix_bone[r.u16(p + 4 + 6 * i)], r.f32(p + 6 + 6 * i)) for i in range(cnt)]
                p += 4 + 6 * cnt
            elif op == 5:
                p += 5
            else:
                raise ValueError('unknown NodeMix op %d' % op)
    return out


def vertex_influences(m, mesh, mix):
    """Per-vertex skinning for a parsed object: joints (n, 4) uint16 and weights (n, 4) float32.
    Rigid vertices (bound to one matrix) get their bone at weight 1; envelope vertices get up to
    four weighted bones."""
    n = len(mesh.matrix_ids)
    joints = np.zeros((n, 4), np.uint16)
    weights = np.zeros((n, 4), np.float32)
    for mid in np.unique(mesh.matrix_ids):
        sel = mesh.matrix_ids == mid
        bone = m.matrix_bone[mid] if mid < len(m.matrix_bone) else -1
        if bone >= 0:
            joints[sel, 0], weights[sel, 0] = bone, 1.0
            continue
        infl = sorted(mix.get(int(mid), [(0, 1.0)]), key=lambda x: -x[1])[:4]
        total = sum(w for _, w in infl) or 1.0
        for k, (b, w) in enumerate(infl):
            joints[sel, k], weights[sel, k] = max(b, 0), w / total
    return joints, weights


def parse_pat0(r, contents):
    """Texture pattern animations (AnmTexPat, PAT0): {material: {layer: {'frames', 'loop', 'keys': [[frame, texture name]]}}}.
    Per material a flags word (one nibble per texture layer: 1 enabled, 2 fixed texture, 4 has texture); each enabled
    layer has either a fixed texture index or an offset to a table of (frame, texture index, palette index) keys."""
    out, by_anim = {}, {}
    for _name, o in contents.get('AnmTexPat(NW4R)', []):
        tex_off, frames = r.s32(o + 0x14), r.u16(o + 0x30)
        ntex, loop = r.u16(o + 0x34), r.u32(o + 0x38)
        names = [r.string(o + tex_off + r.s32(o + tex_off + 4 * i)) for i in range(ntex)]
        for mname, po in r.dict(o + r.u32(o + 0x10)):
            flags, slot = r.u32(po + 4), 0
            for layer in range(8):
                nib = flags >> (4 * layer) & 15
                if not nib & 1:
                    continue
                if nib & 2:
                    keys = [[0, names[r.u16(po + 8 + 4 * slot)]]]
                else:
                    t = po + r.s32(po + 8 + 4 * slot)
                    n = r.u16(t)
                    keys = [[round(r.f32(t + 8 + 8 * i), 3), names[r.u16(t + 12 + 8 * i)]] for i in range(n)]
                slot += 1
                out.setdefault(mname, {})[layer] = {'frames': frames, 'loop': bool(loop), 'keys': keys}
                by_anim.setdefault(_name, {}).setdefault(mname, {})[layer] = out[mname][layer]
    out['PAT0_BY_ANIM'] = by_anim    # several models of one file can each have their own pattern animation for the same material name (the boos' three looks)
    return out


def draw_list(m):
    """[(object index, material index, translucent)] from the DrawOpa/DrawXlu definitions."""
    r = m.r
    out = []
    for name, o in m.sections[Mdl0.DEFS]:
        if name not in ('DrawOpa', 'DrawXlu'):
            continue
        p = o
        while r.u8(p) == 4:
            out.append((r.u16(p + 3), r.u16(p + 1), name == 'DrawXlu'))
            p += 8
    return out



# --------------------------------------------------------------------------------------------
# glTF writer
# --------------------------------------------------------------------------------------------

def material_konst(r, mat_off):
    """TEV colour registers C0-C2 of a material as 0-255 [r, g, b, a] (BP 0xE0-0xE7 writes with bit 23 clear)."""
    regs = {}
    for reg, w in _bp_writes(r.d[mat_off:mat_off + r.u32(mat_off)]):
        if 0xE0 <= reg <= 0xE7 and not w >> 23 & 1:
            regs[reg] = w
    out = {}
    for k in range(4):
        lo, hi = regs.get(0xE2 + 2 * k), regs.get(0xE3 + 2 * k)
        if lo is None or hi is None:
            continue
        out[k] = [lo & 0x7FF, hi >> 12 & 0x7FF, hi & 0x7FF, lo >> 12 & 0x7FF]
    return out


def material_ind_matrix(r, mat_off):
    """Indirect matrix 0 (BP 0x06-0x08): ([[m00, m01, m02], [m10, m11, m12]], scale exponent)."""
    v = {reg: w for reg, w in _bp_writes(r.d[mat_off:mat_off + r.u32(mat_off)]) if 0x06 <= reg <= 0x08}
    def s11(x):
        return (x - 2048 if x >= 1024 else x) / 1024.0
    a, b, c = v.get(6, 0), v.get(7, 0), v.get(8, 0)
    m = [[s11(a & 0x7FF), s11(b & 0x7FF), s11(c & 0x7FF)], [s11(a >> 11 & 0x7FF), s11(b >> 11 & 0x7FF), s11(c >> 11 & 0x7FF)]]
    scale = (a >> 22 & 3) | (b >> 22 & 3) << 2 | (c >> 22 & 3) << 4
    return m, scale - 17


def png_data_url(img):
    buf = io.BytesIO()
    img.save(buf, 'PNG')
    return 'data:image/png;base64,' + base64.b64encode(buf.getvalue()).decode()


def custom_material(r, mat_off, mt, textures, srt_map):
    """Effect materials whose TEV network (indirect textures, several layers) is rebuilt by a shader on the page:
    lava ('lava': ind-warped ramp texture + lava texture) and the fire pole. Constants come from the file."""
    # ef_hpipeBoard (DK Summit's zipper/half-pipe boost strip): decoded its actual TEV color stages
    # (BP 0xC0-0xC3) directly -- stage 0 is a plain texc of layer 1 (rainbowBlueMrr, a 64x8 ramp) into
    # color_prev; stage 1 is texc(layer 0 = arrowShMrr) + texa(layer 0) * color_prev, i.e. the final
    # colour is arrowShMrr.rgb + arrowShMrr.a * rainbowBlueMrr.rgb, both fetched through the same
    # indirect-warp offset as the lava materials (layer 1 doubles as the indirect source, same as lava's
    # convention). layer 2 (bumpGrad) never appears in the colour stages and is unused.
    # ef_sea (ocean water, e.g. Daisy Circuit) and its retro-track twin a_water/a0_water (old_desert_ds,
    # old_donkey_64, old_garden_ds, old_town_ds, old_sherbet_64): layer 0 (wave bump texture) and layer 1
    # (indirect scroll source) follow the lava convention; layer 2 is a projected glow highlight. Decoded
    # TEV stages, both variants share stage0 = mix(K0, white, glow.rgb) (glow brightens the warm K0 base
    # toward white); they differ in stage1: ef_sea chains stage0 back in -- mix(K0, stage0, wave.rgb) +
    # stage0 -- while a_water/a0_water blend toward a fixed K2 colour instead -- mix(K0, K2, wave.rgb) +
    # stage0. Both texture reads go through the same indirect warp offset as lava. Previously unhandled,
    # these fell back to the plain textured path and showed the raw 64x64 wave texture unblended as a
    # harsh black/white pattern.
    # water0_HIG (GCN Mario Circuit puddles): single TEV stage, decoded directly from BP 0xC0 --
    # a=konst (resolved through KCSEL, not a plain K-register -- see _resolve_kcsel) b=c0 c=texc
    # d=texc, i.e. out = mix(kcsel0, c0 + 1.0, wave.rgb). layer 0 (yo_wave_tex) is the visible
    # texture, layer 1 (yo_wave_warp) is a single-channel (no real alpha) indirect ripple source,
    # same convention as the fire pole's glow lookup. Previously unhandled, showed the flat
    # undistorted wave texture with no ripple warp and no real colour tint.
    # GCN Peach Beach's ocean (Psea.brres, a separate placed object -- not part of course_model.brres
    # at all, which is why its sand floor looked bone dry: none of its 5 sub-models' materials were in
    # this dict, so every one fell through to the plain-textured path). Of its 5 layers (sand floor,
    # depth tint, foam highlight, ripple, specular glint), two were decoded and handled here:
    # tex_tx_v (the ripple layer, yo_wave_tex + yo_wave_warp indirect source) turned out to be the
    # exact same single-stage KCSEL-resolved formula as water0_HIG (same resolved blue constant too),
    # so it's just another 'puddle'. dark_v_x (the translucent depth tint over the sand) is a flat
    # TEV output -- a=konst(c0) b=c0 c=texc d=zero reduces to a constant b=a regardless of c, so colour
    # is always c0 (a pale cyan) -- with the per-pixel variation coming only from this layer's own
    # alpha channel (yo_wave_soko), not its RGB. The other 3 layers (sand floor, no-texture foam
    # highlight, dual-texture specular glint) are left unhandled for now.
    # lambert5 (GCN Peach Beach's boost/dash panel): a generic Maya-export name, since GCN retro ports
    # keep their original material names instead of the Wii convention -- structurally the same core
    # formula as ef_dushBoard (arrow texture x 3/4 konst, ADD an environment-mapped rainbow highlight),
    # confirmed via the real TEV stages (0xC0: texc(ef_arrowGradS) x konst(0.75); 0xC2: cprev +
    # texc(ef_rainbowRed2) sampled on a generated/view-normal texcoord row, same category as
    # ef_dushBoard's env layer) -- PLUS a third layer ef_dushBoard doesn't have: ef_arrowBumpS is wired
    # only into the indirect-texture unit (never a TEV colour stage), warping the arrow sample the same
    # way lava's ramp texture warps its own. Previously unhandled (a different name than ef_dushBoard's
    # exact-match special case), it fell through to the plain path showing the raw arrow-gradient mask
    # texture alone -- a flat yellow/black striped look, with no rainbow highlight and no bump wobble.
    kinds = {'ef_volSurface': 'lava', 'ef_volFall': 'lava', 'ef_flamePoleMat': 'flamePole',
             'ef_flamePlaneMat': 'flamePlane', 'ef_hpipeBoard': 'hpipe', 'ef_sea': 'water',
             'a_water': 'waterK2', 'a0_water': 'waterK2', 'water0_HIG': 'puddle', 'tex_tx_v': 'puddle',
             'ef_lake': 'water', 'lake': 'lake05', 'm_koopaBallPl': 'fireMix', 'm_koopaBallSp': 'fireMix', 'mat_fireBplane': 'fireMix', 'bom': 'matcap', 'efPocha0': 'pochaMix', 'efPocha1': 'pochaMix', 'kp_sky_v': 'skyK', 'taki02': 'taki', 'bobleSp': 'fireMix', 'bobleKasa': 'fireMix', 'HeyhoBall': 'ballBlink', 'z1_road': 'roadRef', 'z2_road': 'roadRef', 'a_star': 'roadRef', 'ef_ring': 'ringTint',
             'dark_v_x': 'flatTint', 'sand_tx_v_x': 'sandTint', 'nami_tx_v_x': 'foam', 'spc_tx_v': 'spec', 'lambert5': 'dushBump', 'a1_dushBoard': 'dushBump', 'ef_dushBoard': 'dushBump'}
    # Psea's five layers (decoded in full from their TEV/blend registers; draw order = model order). Each is
    # a single TEV stage whose alpha is a flat KONST alpha (K0.A, via KSEL), blended with its own GX blend mode
    # (BP 0x41): sand = texc*K0 (dk_sky is solid white, so just a grey tint) / alpha; dark_v_x = mix(K0, C0,
    # texc) (yo_wave_soko is solid black, so just K0) / alpha; nami = K0+1 (clamps white) with dst*(src+a);
    # tex_tx_v = the puddle formula with dst*(src+1-a); spc_tx_v = 4*wave0*wave1 added on top (no indirect).
    # NB dark_v_x used to be decoded as the pale-cyan C0 with the texture's alpha -- wrong on both counts.
    # water0_HIG (Mario Circuit puddles): same flat K0.A alpha stage, plain alpha blend (BP 0x41 = 0x34A1), K0.A = 0.5.
    psea = {'water0_HIG': ('alpha', 0), 'sand_tx_v_x': ('alpha', 1), 'dark_v_x': ('alpha', 2), 'nami_tx_v_x': ('foam', 3),
            'tex_tx_v': ('mulA', 4), 'spc_tx_v': ('add', 5)}
    # ef_lake (Dry Dry Ruins): the same two-stage TEV as ef_sea above (stage0 = mix(C0, white, glow), stage1 = mix(C0, stage0, wave) + stage0),
    # with a blue C0 -- it was simply never listed. 'lake' (DS Cheep Cheep Falls) is a close variant: stage0 is scaled x0.5 and stage1 is
    # mix(C0, white, wave) + stage0 (kind 'lake05'). Only that material with the glow texture in layer 2 qualifies ('lake' is a common name).
    if mt.name == 'lake' and (len(mt.textures) < 3 or mt.textures[2]['name'] != 'ef_prj_glow'):
        return None
    if mt.name not in kinds:
        return None
    if kinds[mt.name] == 'taki' and (not mt.textures or mt.textures[0]['name'] != 'fa_taki02'):
        return None
    # N64 Sherbet Land's a0_water is the 'lake' network (see above) with HALF instead of ONE in stage 0: stage0 = 0.5 * mix(C0, 0.5, glow), stage1 = CPREV + mix(C0, 1, wave)
    water_half = False
    if kinds[mt.name] == 'waterK2':
        sh_w = mat_off + r.s32(mat_off + 0x28)
        cw = [v for g, v in _bp_writes(r.d[sh_w:sh_w + r.u32(sh_w)]) if g in (0xC0, 0xC2)]
        if len(cw) >= 2 and (cw[0] >> 8 & 0xF) == 0xD and (cw[0] >> 20 & 3) == 3 and (cw[1] & 0xFFFF) == 0x2C80:
            kinds = dict(kinds); kinds[mt.name] = 'lake05'; water_half = True
    k = material_konst(r, mat_off)
    m, exp = material_ind_matrix(r, mat_off)
    # dushBump reorders the raw [rainbow, bump, arrow] texture layers to [arrow, bump, rainbow] so the
    # shared shader's tA/tInd/tB convention (visible texture, indirect source, overlay) lines up without
    # needing a fourth index parameter -- layer_srt/anim stay keyed by each texture's ORIGINAL index.
    order = [2, 1, 0] if kinds[mt.name] == 'dushBump' else list(range(len(mt.textures)))
    names = [mt.textures[i]['name'] for i in order]
    anim = (srt_map or {}).get(mt.name, {})
    layers = []
    for pos, i in enumerate(order):
        base = [round(x, 5) for x in mt.layer_srt[i]]
        a = anim.get(i)
        layer = dict(a, base=base) if a else {'frames': 0, 'scale': [None, None], 'rot': None, 'trans': [None, None], 'base': base}
        # dushBump's arrow+bump layers (reordered positions 0/1, the rainbow overlay at position 2
        # samples a generated/view-normal coordinate and is unaffected) share ef_dushBoard's exact
        # texture pair (ef_arrowGradS/ef_arrowBumpS) and so the SAME already-measured reverse-wound
        # mesh UV (see registerTexAnim()'s ef_dushBoard flip in index.html) -- without correcting for
        # it here too, the shared lava-family srtMatrix()'s Maya-style negation scrolls the arrows
        # backwards through the warp.
        if kinds[mt.name] == 'dushBump' and pos < 2:
            layer = dict(layer, flip=True)
        layers.append(layer)
    # Bowser's Castle: 'bom' (the fireball's iron bomb) = C0 (blue-grey) x a sphere-map highlight (bom_01, generated from the view normal) x light;
    # the flame planes ('fireMix'): stage0 = mix(C1, C0, tex0 warped by the indirect layer), stage1 (if any) adds mix(C1, C0, tex1); alpha = the
    # textures' alpha; drawn additively. mat_fireBplane lists its colours the other way round (a=C0, b=C1), so they are swapped here.
    if kinds[mt.name] == 'fireMix':
        ind27 = [v for g, v in _bp_writes(r.d[mat_off + r.s32(mat_off + 0x28):][:r.u32(mat_off + r.s32(mat_off + 0x28))]) if g == 0x27]
        k = dict(k)
        if mt.name in ('mat_fireBplane', 'bobleSp'):
            k[0], k[1] = k[1], k[0]
    kcsel0 = _resolve_kcsel(r, mat_off, 0) if kinds[mt.name] in ('puddle', 'dushBump', 'flatTint', 'sandTint', 'foam', 'skyK', 'roadRef', 'ringTint', 'taki', 'ballBlink') else None
    out = {'kind': kinds[mt.name], 'c0': k.get(0, [255, 255, 255])[:3], 'c1': k.get(1, [255, 255, 255])[:3], 'c2': k.get(2, [0, 0, 0])[:3],
           'kcsel0': kcsel0, 'm': m, 'exp': exp, 'size': textures[names[0]].size[0] if names else 1,
           'wraps': [list(mt.textures[i]['wrap']) for i in order], 'layers': layers,
           'textures': [png_data_url(textures[n]) for n in names]}
    if kinds[mt.name] == 'fireMix':
        out['ind'] = (ind27[-1] & 7) if ind27 else 1
        sd = r.d[mat_off + r.s32(mat_off + 0x28):][:r.u32(mat_off + r.s32(mat_off + 0x28))]
        out['hasB'] = 1 if len([1 for g, v in _bp_writes(sd) if g == 0xC2]) else 0
        out['blend'], out['order'] = 'add', 2
    if kinds[mt.name] == 'skyK':
        # Bowser's Castle's sky: stage 0 = (mix(C0, tex0, vertex colour) + 0.5) / 2, stage 1 = (K0 - mix(prev, tex1, 0.5)) / 2 (subtract, clamped): a dark red sky
        # whose bright texture areas are darkest. K0 is animated (CLR0 'vrcorn', 960 frames, a slow pulse): out['clr'] = its per-frame colour.
        out['clr'] = clr0_konst_table(r, mt.name)
    if kinds[mt.name] == 'roadRef':
        # Rainbow Road's road (z1_road / z2_road): stage 0 = 2 * ref01 * K (an environment map, warped per tile by the road_indirect block pattern), stage 1 =
        # (stage0 + road texture * vertex colour) / 2 -- the road is HALVED and a grey reflection added, which is what makes the tiles pastel and glassy
        # (the old plain 'road * colour * 2' was oversaturated). z2_road is alpha-blended over z1.
        # c2 = the two stage scales (stage 0, stage 1): road 2 and 1/2; the cannon's star (a_star, same network) 1 and 2
        # c2 = (stage 0 scale, stage 1 scale, star flag), straight from the TEV (BP 0xC0/0xC2 scale bits): road 2 and 1/2; the cannon's star (a_star, same network) 1 and 2.
        # The reflection of the road is a PROJECTION texgen (screen-space, effect matrix identity, per noclip's BRRES reader); the star's is an environment map.
        out['c2'] = [255, 510, 255] if mt.name == 'a_star' else [510, 242, 0]
        if mt.name != 'a_star':
            out['kalpha'] = 0.55   # the road's reflection weight: measured against Dolphin (the decoded 1/2 + 1/2 sum came out ~25% too dark and desaturated)
        out['ind'] = 2
        out['size'] = textures[names[1]].size[0] if len(names) > 1 else out['size']
        if mt.name == 'z2_road':
            out['blend'], out['order'] = 'alpha', 1
    if kinds[mt.name] == 'ringTint':
        # ef_ring (the glow rings round Rainbow Road's arrows): TEXC * KONST * 4 with KONST = pure green (KSEL), added with the texture's alpha -- the page drew the blue texture as is
        out['c2'] = [1020, 0, 0]
        out['blend'], out['order'] = 'add', 2
    if kinds[mt.name] == 'taki':
        # DS Yoshi Falls' waterfall (taki02): stage 0 = TEXC + lerp(KONST, C0, TEXC) (KONST a blue, C0 white), alpha = the constant KONST alpha (0.39), plain alpha blend.
        # Drawn as the page's plain texture it came out black. Its UV scroll uses the exact Maya texture matrix (see srtMatrix).
        out['blend'], out['order'] = 'alpha', 1
        out['kalpha'] = _resolve_kasel(r, mat_off, 0)
    if kinds[mt.name] == 'ballBlink':
        # GBA Shy Guy Beach's cannonball: out = mix(C1, TEXC(bom_01 sphere map), C0), C1 animated by CLR0 'HeyhoBallGBA_blink' (a red pulse): the red flashing
        out['clr'] = clr0_konst_table(r, mt.name)
    if kinds[mt.name] == 'pochaMix':
        # Bowser's Castle's lava splash (pochaYogan): stage 0 = mix(C1, C0, tex0), stage 1 adds tex1 * C2; the outer shell (efPocha1) takes its alpha from tex0
        out['hasB'] = 1 if mt.name == 'efPocha1' else 0
        if mt.name == 'efPocha1':
            out['blend'], out['order'] = 'alpha', 3
    if water_half:
        out['hasB'] = 1
        if mt.xlu:
            ka = _resolve_kasel(r, mat_off, 1)
            if ka is not None:
                out['blend'], out['kalpha'], out['order'] = 'alpha', ka, 1
    if mt.name in psea:
        out['blend'], out['order'] = psea[mt.name]
        out['kalpha'] = _resolve_kasel(r, mat_off, 0)
    elif kinds[mt.name] in ('water', 'waterK2') and mt.xlu:
        # Translucent seas (Moonview Highway's lake): the last TEV alpha stage is a constant KONST alpha (0.114 there), alpha-blended over
        # whatever lies beneath -- drawn opaque, the lake came out as a solid mint-green slab.
        ka = _resolve_kasel(r, mat_off, 1)
        if ka is not None:
            out['blend'], out['kalpha'], out['order'] = 'alpha', ka, 1
    return out


def clr0_konst_table(r, mat_name):
    """Per-frame [r, g, b] of the (first) colour track of the CLR0 animation entry for a material, or None. Layout (NW4R CLR0 v4): the entry holds the
    name offset, a flags word (2 bits per target), then per target a flags word and an offset (relative to its own position) to RGBA frames."""
    contents = brres_contents(r)
    for _name, off in contents.get('AnmClr(NW4R)', []):
        frames = r.u32(off + 0x20) >> 16
        for mn, mo in r.dict(off + r.s32(off + 0x10)):
            if mn == mat_name:
                tbl = mo + 0xC + r.u32(mo + 0xC)
                return [list(r.d[tbl + 4 * i: tbl + 4 * i + 3]) for i in range(frames)]
    return None


def light_mask_info(r, mat_off, mt, textures):
    """Moonview Highway's roads, walls and grass carry a second texture, 'Mi_LightMask' (a soft radial glow), that the game projects onto
    the ground around the kart. Decoded TEV (road): stage 0 = mix(KONST, C0, mask) (a=KONST b=C0 c=TEXC, the mask), stage 1 =
    (CPREV + RASC * base) x 2 -- i.e. the glow ADDS mix(K, C0, mask) to the lit base colour before the x2. Grass (siba00) also multiplies
    the glow by the vertex alpha (stage 1 = CPREV * RASA). The projection matrix is built by the game at runtime (not in the file)."""
    idx = next((i for i, t in enumerate(mt.textures) if 'LightMask' in t['name'] and t['name'] in textures), None)
    if idx is None:
        return None
    k = material_konst(r, mat_off)
    sh = mat_off + r.s32(mat_off + 0x28)
    stage1 = [v for reg, v in _bp_writes(r.d[sh:sh + r.u32(sh)]) if reg == 0xC2]
    alpha_scale = bool(stage1) and (stage1[0] >> 12 & 15, stage1[0] >> 8 & 15, stage1[0] >> 4 & 15, stage1[0] & 15) == (15, 0, 11, 15)
    return {'texture': png_data_url(textures[mt.textures[idx]['name']]), 'c0': [round(x / 255, 4) for x in k.get(0, [0, 0, 0])[:3]],
            'k': _resolve_kcsel(r, mat_off, 0) or [0, 0, 0], 'alphaScale': alpha_scale}


class Gltf:
    def __init__(self):
        self.j = {'asset': {'version': '2.0', 'generator': 'Kinoko brres_to_glb.py'},
                  'scene': 0, 'scenes': [{'nodes': []}], 'nodes': [], 'meshes': [],
                  'materials': [], 'textures': [], 'images': [], 'samplers': [],
                  'accessors': [], 'bufferViews': [], 'buffers': []}
        self.bin = bytearray()
        self.images = {}     # texture name -> image index
        self.materials = {}  # (brres id, material index) -> material index
        self.pat_images = {}  # texture pattern animation frames: name -> image, written next to the glb as pat/<name>.png

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)}
        if target:
            view['target'] = target
        self.bin += data
        self.j['bufferViews'].append(view)
        return len(self.j['bufferViews']) - 1

    def _accessor(self, arr, kind, ctype=5126, target=34962):
        dtype = {5126: np.float32, 5125: np.uint32, 5123: np.uint16}[ctype]
        arr = np.ascontiguousarray(arr, dtype=dtype)
        acc = {'bufferView': self._view(arr.tobytes(), target), 'componentType': ctype,
               'count': len(arr), 'type': kind}
        if (kind == 'VEC3' and ctype == 5126) or kind == 'SCALAR' and target is None:
            acc['min'] = np.atleast_1d(arr.min(0)).tolist()
            acc['max'] = np.atleast_1d(arr.max(0)).tolist()
        self.j['accessors'].append(acc)
        return len(self.j['accessors']) - 1

    def image(self, name, pil):
        if name not in self.images:
            buf = io.BytesIO()
            pil.save(buf, 'PNG', optimize=True)
            self.j['images'].append({'name': name, 'mimeType': 'image/png',
                                     'bufferView': self._view(buf.getvalue())})
            self.images[name] = len(self.j['images']) - 1
        return self.images[name]

    def _texture_index(self, ref, img):
        self.j['samplers'].append({'wrapS': WRAP.get(ref['wrap'][0], 10497),
                                   'wrapT': WRAP.get(ref['wrap'][1], 10497)})
        self.j['textures'].append({'source': self.image(ref['name'], img),
                                   'sampler': len(self.j['samplers']) - 1})
        return len(self.j['textures']) - 1

    def material(self, key, mat, textures, track=None):
        """textures: {name: PIL image} of the BRRES the material belongs to. track: None for
        vehicles (first texture layer), or choose_layers()'s (base, shadow) for course materials."""
        if key in self.materials:
            return self.materials[key]
        m = {'name': mat.name, 'doubleSided': mat.cull == 0,
             'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': 1.0}}
        base_i = track[0][0] if track else 0
        base = mat.textures[base_i] if len(mat.textures) > base_i else None
        # Daisy Circuit's sky material (senior_sky, vrcorn_model.brres) references senior_VR2 -- a dark
        # mauve/night-toned cloud layer -- while the file's own texture pool also carries senior_VR, an
        # unused warm orange sunset gradient that matches the real game's actual sky exactly (same cloud
        # layout, just the intended palette). No other material, SRT0 scroll, or PAT0 pattern animation
        # in the file ever references senior_VR, and it's the only track in the game with an orphaned
        # sky texture like this (checked all 32) -- looks like the shipped material was simply pointed
        # at the wrong one of the two. Confirmed by direct visual comparison against real gameplay.
        if base and base['name'] == 'senior_VR2':
            base = dict(base, name='senior_VR')
        # choose_layers() flags a material's second texture layer as a baked shadow/AO map purely from
        # its name/role, without checking how the TEV network actually combines it in -- three.js's
        # default AO handling (straight multiply) only matches one of the two real shapes
        # material_shadow_color() knows about ('lerp'; see its docstring for 'additive', the other one).
        # Both get the correct treatment via the custom shadowed shader branch in index.html, keyed off
        # shadowColor/shadowMode, so only materials matching one of those two verified formulas get
        # occlusionTexture wired up at all -- anything else is left alone rather than risk three.js's
        # default straight-multiply AO blacking out geometry whose real formula we haven't decoded.
        if track and track[1] and mat.textures[track[1][0]]['name'] in textures and mat.shadow_color is not None:
            ao = mat.textures[track[1][0]]
            m['occlusionTexture'] = {'index': self._texture_index(ao, textures[ao['name']]), 'texCoord': 1}
        if base and base['name'] in textures:
            img = textures[base['name']]
            ref = base
            # GX intensity textures (I4/I8) have no real alpha channel -- the hardware just replicates
            # the intensity value into alpha too, which is what fades a glow's edges out when it's added
            # over the scene, or what makes a soft alpha-blended sprite (smoke, clouds) fade at the
            # edges instead of showing as an opaque rectangle. This applies whenever the material
            # actually uses alpha (additive, or a regular translucent material), not just additive --
            # it used to be additive-only, which left translucent I4/I8 sprites (e.g. the mine's chimney
            # steam, ef_smkPlane) rendering as solid opaque blobs with a hard black box around them.
            if img.info.get('gxfmt') in (0, 1) and (getattr(mat, 'blend', None) == 'add' or mat.xlu):
                img = img.copy()
                img.putalpha(img.getchannel('R'))
                ref = dict(base, name=base['name'] + '#glow')
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': self._texture_index(ref, img)}
            if mat.xlu:
                m['alphaMode'] = 'BLEND'
            else:
                cutoff = getattr(mat, 'alpha_compare', None)
                if cutoff is not None and img.mode == 'RGBA' and img.getextrema()[3][0] < 255:
                    m['alphaMode'], m['alphaCutoff'] = 'MASK', cutoff
                elif getattr(mat, 'alpha_blend', False) and img.mode == 'RGBA' and img.getextrema()[3][0] < 255:
                    m['alphaMode'] = 'BLEND'
        if getattr(mat, 'blend', None) == 'add':
            m['alphaMode'] = 'BLEND'
            m['extras'] = {'blend': 'add'}
        if track and getattr(mat, 'shadow_color', None):
            m.setdefault('extras', {})['shadowColor'] = mat.shadow_color
            m['extras']['shadowMode'] = mat.shadow_mode
        if track and getattr(mat, 'tev_scale', 1) != 1:
            m.setdefault('extras', {})['tevScale'] = mat.tev_scale
        if getattr(mat, 'srt_anim', None):
            m.setdefault('extras', {})['srt'] = mat.srt_anim
        if track and base and base['name'] in textures:
            m.setdefault('extras', {})['mips'] = textures[base['name']].info.get('mips', 1)
        if getattr(mat, 'custom', None):
            m.setdefault('extras', {})['custom'] = mat.custom
        if getattr(mat, 'light_mask', None):
            m.setdefault('extras', {})['lightMask'] = mat.light_mask
        if getattr(mat, 'env', None):
            m.setdefault('extras', {})['env'] = mat.env
        if getattr(mat, 'sky_overlay', None):
            m.setdefault('extras', {})['skyOverlay'] = mat.sky_overlay
        if getattr(mat, 'vtx_mix', None):
            m.setdefault('extras', {})['vtxMix'] = mat.vtx_mix
        if getattr(mat, 'lit_maps', None):
            m.setdefault('extras', {})['litMaps'] = mat.lit_maps
        if getattr(mat, 'pat_anim', None):
            pa = mat.pat_anim
            m.setdefault('extras', {})['pat'] = pa
            for _f, n in pa['keys']:
                if n in textures:
                    self.pat_images[n] = textures[n]
        self.j['materials'].append(m)
        self.materials[key] = len(self.j['materials']) - 1
        return self.materials[key]

    def add_node(self, name, primitives):
        """primitives: [(positions, normals, uvs, colors, triangles, material index)]."""
        self.j['nodes'].append({'name': name, 'mesh': self.mesh(name, primitives)})
        self.j['scenes'][0]['nodes'].append(len(self.j['nodes']) - 1)

    def mesh(self, name, primitives, joints=None):
        """Mesh index. joints: per primitive, the joint index of each vertex (rigid skinning)."""
        prims = []
        for i, (pos, nrm, uv, clr, tris, mat) in enumerate(primitives):
            attrs = {'POSITION': self._accessor(pos, 'VEC3')}
            if joints is not None:
                if isinstance(joints[i], tuple):       # (joints (n, 4), weights (n, 4)): blended vertices
                    j, w = joints[i]
                else:                                   # one bone per vertex
                    j = np.zeros((len(pos), 4), np.uint16)
                    j[:, 0] = joints[i]
                    w = np.zeros((len(pos), 4), np.float32)
                    w[:, 0] = 1
                attrs['JOINTS_0'] = self._accessor(j, 'VEC4', 5123)
                attrs['WEIGHTS_0'] = self._accessor(w, 'VEC4')
            if nrm is not None:
                attrs['NORMAL'] = self._accessor(nrm, 'VEC3')
            if uv is not None:
                uvs = uv if isinstance(uv, list) else [uv]  # [base uv, shadow-map uv]
                for k, u in enumerate(uvs):
                    attrs['TEXCOORD_%d' % k] = self._accessor(u, 'VEC2')
            if clr is not None and not np.allclose(clr, 1):
                attrs['COLOR_0'] = self._accessor(clr, 'VEC4')
            prims.append({'attributes': attrs, 'material': mat,
                          'indices': self._accessor(tris.reshape(-1), 'SCALAR', 5125, 34963)})
        self.j['meshes'].append({'name': name, 'primitives': prims})
        return len(self.j['meshes']) - 1

    def save(self, path):
        if self.pat_images:
            d = os.path.join(os.path.dirname(os.path.abspath(path)), 'pat')
            os.makedirs(d, exist_ok=True)
            for n, img in self.pat_images.items():
                img.save(os.path.join(d, n.replace('#', '_') + '.png'))
        self.j['buffers'] = [{'byteLength': len(self.bin)}]
        for k in [k for k, v in self.j.items() if v == []]:
            del self.j[k]
        js = json.dumps(self.j, separators=(',', ':')).encode()
        js += b' ' * (-len(js) % 4)
        binary = bytes(self.bin) + b'\0' * (-len(self.bin) % 4)
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binary)))
            f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
            f.write(struct.pack('<II', len(binary), 0x004E4942) + binary)


def model_primitives(gltf, key, m, textures, bone_world=None, track=False, srt_map=None, skin=None, mix=None, sky=False):
    """Primitives for every draw call of an MDL0, in model space. track=True: course mode, where
    each material's diffuse and baked shadow-map layers are picked with choose_layers() and
    their UV sets become TEXCOORD_0 and TEXCOORD_1. sky=True (vrcorn_model.brres): layers are picked
    with choose_sky_layers() instead, and a second ADDED layer goes out as extras.skyOverlay rather
    than the AO-style TEXCOORD_1 shadow map (see choose_sky_layers())."""
    mats = parse_materials(m)
    objs = {r_index: o for r_index, o in ((m.r.u32(o + 0x3C), o) for _, o in m.sections[Mdl0.OBJS])}
    prims = []
    mix = node_mix(m) if skin is not None and mix is None else mix   # skin: list to fill with (joints, weights)
    for obj_i, mat_i, _ in draw_list(m):
        mesh = parse_object(m, objs[obj_i])
        if mesh.positions is None or not len(mesh.triangles):
            continue
        pos, nrm = model_space(m, mesh, bone_world)
        tris = mesh.triangles[:, [0, 2, 1]]  # GX front faces are clockwise; glTF's are CCW
        if track:
            mt = mats[mat_i]
            sel = choose_sky_layers(m.r, mt.off, mt) if sky else choose_layers(mt, textures)
            (base_layer, base_uv), shadow = sel
            uvs = mesh.uv_sets.get(base_uv)
            layer_srt = mt.layer_srt[base_layer] if base_layer < len(mt.layer_srt) else [1, 1, 0, 0, 0]
            anim = (srt_map or {}).get(mt.name, {}).get(base_layer)
            # An animated layer keeps raw UVs: the page rebuilds its whole texture matrix each frame
            # from the animation (falling back to this layer's own values for channels it leaves out)
            mt.srt_anim = dict(anim, base=[round(x, 5) for x in layer_srt]) if anim else None
            _pa = (srt_map or {}).get('PAT0_ANIMS', {})
            mt.pat_anim = ((_pa.get('PAT0_BY_ANIM', {}).get(m.name, {}).get(mt.name)) or _pa.get(mt.name, {})).get(base_layer)
            mt.env = None
            mt.custom = None
            mt.sky_overlay = None
            acc_uv_set = None
            # Rainbow Road's sky (vr_starsky): stage 1 is exactly CPREV + TEXC * KONST (BP 0xC2 = a zero, b KONST, c TEXC, d CPREV), i.e. the accent layer is added
            # at the full KONST weight and with its own UV set (the tiled star field); other skies keep the 0.5 / shared-UV approximation
            sh_off = mt.off + m.r.s32(mt.off + 0x28)
            sh_bp = dict(_bp_writes(m.r.d[sh_off:sh_off + m.r.u32(sh_off)])) if sky else {}
            sky_add_k = _resolve_kcsel(m.r, mt.off, 1) if sky and (sh_bp.get(0xC2, 0) & 0xFFFF) == 0xFE80 else None
            if sky:
                # The accent layer (choose_sky_layers()'s second return value) is ADDED on top of the
                # base, scaled by its own animated SRT -- not baked into TEXCOORD_1 like a shadow map,
                # since it needs its own independently-scrolling UV, not the base layer's.
                if shadow:
                    acc_layer, _acc_uv = shadow
                    acc_name = mt.textures[acc_layer]['name']
                    if acc_name in textures:
                        acc_srt = mt.layer_srt[acc_layer] if acc_layer < len(mt.layer_srt) else [1, 1, 0, 0, 0]
                        acc_anim = (srt_map or {}).get(mt.name, {}).get(acc_layer)
                        mt.sky_overlay = {
                            'texture': png_data_url(textures[acc_name]),
                            'weight': sky_add_k[0] if sky_add_k else 0.5,
                            'screen': bool(sky and sky_is_screen(m.r.d[sh_off:sh_off + m.r.u32(sh_off)])),
                            'wrap': list(mt.textures[acc_layer]['wrap']),
                            'srt': dict(acc_anim, base=[round(x, 5) for x in acc_srt]) if acc_anim else
                                   {'frames': 0, 'scale': [None, None], 'rot': None, 'trans': [None, None], 'base': [round(x, 5) for x in acc_srt]},
                        }
                if shadow and shadow[1] in mesh.uv_sets and mt.sky_overlay and sky_add_k:
                    mt.sky_overlay['uv1'] = True      # the accent layer has its own UV set: it goes out as TEXCOORD_1
                    acc_uv_set = shadow[1]
                shadow = None
                if mt.name == 'kp_sky_v':
                    mt.custom = custom_material(m.r, mt.off, mt, textures, srt_map)
            else:
                mt.custom = custom_material(m.r, mt.off, mt, textures, srt_map)
                mt.light_mask = light_mask_info(m.r, mt.off, mt, textures)
                # two-layer materials whose second TEV stage is TEXC * CPREV (a=ZERO b=TEXC c=CPREV d=ZERO): the second texture MULTIPLIES the first (e.g. GCN Peach
                # Beach's rock arch: pc_iwa_p2 x pc_sima3, the latter with its own rotated SRT). Sent as a 'mul' overlay (see toUnlit in the page).
                sh_o = mt.off + m.r.s32(mt.off + 0x28)
                sd_o = m.r.d[sh_o:sh_o + m.r.u32(sh_o)]
                cols_o = [v for g, v in _bp_writes(sd_o) if g in (0xC0, 0xC2, 0xC4, 0xC6)]
                reg_o = [(l, uv) for l, uv in getattr(mt, 'regular', []) if l < len(mt.textures)]
                # lit-sphere shading (cataquacks): stage 0 = RASC + RASA * TEXC of a diffuse 'lm_0' sphere map, stage 1 = C1 + CPREV * TEXC(base), plus a specular 'lm_1'
                # sphere map; both are indexed by the view-space normal (ENV_CAMERA). The ramps are not drawable layers, so they travel as extras.litMaps.
                names_o = [t['name'] for t in mt.textures]
                mt.lit_maps = None
                if 'lm_0' in names_o and 'lm_1' in names_o and 'lm_0' in textures and 'lm_1' in textures:
                    mt.lit_maps = {'diffuse': png_data_url(textures['lm_0']), 'spec': png_data_url(textures['lm_1'])}
                if (mt.custom is None and len(cols_o) == 2 and (cols_o[1] & 0xFFFF) == 0xF80F and len(reg_o) == 2 and reg_o[1][0] != base_layer
                        and mt.textures[reg_o[1][0]]['name'] in textures):
                    ml = reg_o[1][0]
                    msrt = mt.layer_srt[ml] if ml < len(mt.layer_srt) else [1, 1, 0, 0, 0]
                    manim = (srt_map or {}).get(mt.name, {}).get(ml)
                    mt.sky_overlay = {'texture': png_data_url(textures[mt.textures[ml]['name']]), 'weight': 1.0, 'mul': True, 'wrap': list(mt.textures[ml]['wrap']),
                                      'srt': dict(manim, base=[round(x, 5) for x in msrt], exact=True) if manim else
                                             {'frames': 0, 'scale': [None, None], 'rot': None, 'trans': [None, None], 'base': [round(x, 5) for x in msrt], 'exact': True}}
            if mt.name == 'ef_dushBoard' and mt.textures[0]['name'] in textures:
                # boost panel: the first layer is an environment map (texgen from the view-space normal, animated
                # by its own SRT) that the TEV adds to the arrow texture x 3/4 (constant colour of stage 0)
                rb = textures[mt.textures[0]['name']].convert('RGB')
                e_anim = (srt_map or {}).get(mt.name, {}).get(0)
                if e_anim:
                    mt.env = {'colors': [list(rb.getpixel((x, 0))) for x in range(rb.size[0])], 'k': 0.75,
                              'srt': dict(e_anim, base=[round(x, 5) for x in mt.layer_srt[0]])}
            if mt.name == 'Press_t' and len(mt.textures) > 1 and mt.textures[1]['name'] in textures:
                # press/crusher glass band: the second layer (k_kankyou02) is a classic matcap-style sphere
                # texture sampled by the view-space normal (not animated), which the TEV adds on top of the
                # base gauge texture -- both terms scaled by vertex colour (stage 0: RASC*TEXC; stage 1:
                # + RASC*TEXC_env). Without it the raw gauge texture (a rainbow/star pattern meant to be
                # mostly washed out by the reflection) shows through at full strength.
                mt.env = {'data': png_data_url(textures[mt.textures[1]['name']].convert('RGB')), 'vcol': True}
            if uvs is not None and not anim and not mt.custom:
                uvs = uvs * np.array(layer_srt[:2])
            if acc_uv_set is not None and uvs is not None:
                uvs = [uvs, mesh.uv_sets[acc_uv_set]]
                sel = (sel[0], None)   # no shadow-map material for the accent layer (it is the page's skyOverlay)
            elif shadow and shadow[1] in mesh.uv_sets and uvs is not None:
                uvs = [uvs, mesh.uv_sets[shadow[1]]]
            else:
                sel = (sel[0], None)   # shadow layer needs its UV set to be present
            mat = gltf.material((key, m.name, mat_i), mats[mat_i], textures, sel)
        else:
            mat = gltf.material((key, m.name, mat_i), mats[mat_i], textures)
            uvs = None if mesh.uvs is None else mesh.uvs * mats[mat_i].uv_scale
        prims.append((pos, nrm, uvs, mesh.colors, tris, mat))
        if skin is not None:
            skin.append(vertex_influences(m, mesh, mix))
    return prims


def brres_textures(r, contents):
    out = {}
    for name, o in contents.get('Textures(NW4R)', []):
        try:
            out[name] = decode_texture(r, o)
        except ValueError as e:
            print('   ! texture %s: %s' % (name, e))
    return out


def surface_below(pos, tris, x, z, y_top):
    """Highest point of a triangle mesh on the vertical line through (x, z), at or below y_top."""
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    e0, e1 = b - a, c - a
    px, pz = x - a[:, 0], z - a[:, 2]
    den = e0[:, 0] * e1[:, 2] - e1[:, 0] * e0[:, 2]
    ok = np.abs(den) > 1e-9
    den = np.where(ok, den, 1)
    u = (px * e1[:, 2] - e1[:, 0] * pz) / den
    v = (e0[:, 0] * pz - px * e0[:, 2]) / den
    y = a[:, 1] + u * e0[:, 1] + v * e1[:, 1]
    y = y[ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (y <= y_top)]
    return y.max() if len(y) else None


def seat_penetration(m, world, body):
    """How far the driver's backside (vertices on the root bone) is sunk below the top of the
    vehicle body under it; 0 if it's above. body: (positions, triangles) of the vehicle body.
    Uses the topmost surface: on some bikes (Mach Bike) the offset sinks the rider below the
    whole seat, so anything near the hip would miss it."""
    root = next((b for b in m.bones if b.name == 'skl_root'), None)
    if root is None:
        return 0.0
    depths = []
    for _, o in m.sections[Mdl0.OBJS]:
        mesh = parse_object(m, o)
        if mesh.positions is None:
            continue
        verts, _ = model_space(m, mesh, world)
        bones = np.array([m.matrix_bone[i] if i < len(m.matrix_bone) else -1 for i in mesh.matrix_ids])
        for x, y, z in verts[bones == root.index][::4]:
            seat = surface_below(body[0], body[1], x, z, np.inf)
            if seat is not None:
                depths.append(seat - y)
    return max(0.0, float(np.percentile(depths, 90))) if len(depths) >= 3 else 0.0


def driver_offset(m, anim, disp, body=None, handle=None):
    """Where the driver's skeleton sits on the vehicle: the character/vehicle offset from
    kartDriverDispParam ([0] y, [1] z). On bikes that offset leaves the rider sunk into the seat
    (checked against in-game screenshots), so they're raised onto it using the neutral pose."""
    offset = np.zeros(3)
    if disp is None:
        return offset
    offset[1], offset[2] = disp[0], disp[1]
    if handle is not None and body is not None:
        world = posed_world(m, anim) if anim else {b.index: b.world.copy() for b in m.bones}
        shift = np.eye(4)
        shift[:3, 3] = offset
        offset[1] += seat_penetration(m, {k: shift @ v for k, v in world.items()}, body)
    return offset


def ik_targets(disp, handle):
    """IK goals the page applies every frame (left side; the right side mirrors x):
    hands on the steering wheel / handlebar grips, feet on the pedals / footpegs.
    Bike hand targets are in the handlebar's space, so they follow it as it steers."""
    if disp is None:
        return None
    return {'hand': list(disp[2:5]) if any(disp[2:5]) else None,
            'foot': list(disp[8:11]) if any(disp[8:11]) else None,
            'handSpace': 'handle' if handle is not None else 'kart'}


def _quat(rot):
    """xyzw quaternion of an NW4R bone rotation (degrees, applied X then Y then Z)."""
    m = srt_matrix((1, 1, 1), rot, (0, 0, 0))[:3, :3]
    t = np.trace(m)
    if t > 0:
        s_ = math.sqrt(t + 1) * 2
        q = [(m[2, 1] - m[1, 2]) / s_, (m[0, 2] - m[2, 0]) / s_, (m[1, 0] - m[0, 1]) / s_, s_ / 4]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s_ = math.sqrt(1 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [s_ / 4, (m[0, 1] + m[1, 0]) / s_, (m[0, 2] + m[2, 0]) / s_, (m[2, 1] - m[1, 2]) / s_]
    elif m[1, 1] > m[2, 2]:
        s_ = math.sqrt(1 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s_, s_ / 4, (m[1, 2] + m[2, 1]) / s_, (m[0, 2] - m[2, 0]) / s_]
    else:
        s_ = math.sqrt(1 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s_, (m[1, 2] + m[2, 1]) / s_, s_ / 4, (m[1, 0] - m[0, 1]) / s_]
    q = np.array(q)
    return q / np.linalg.norm(q)


ANIM_FPS = 60


def add_rigged_driver(g, m, tex, r, anims, offset, extras):
    """Driver as a skinned mesh: a 'driver' root node (at the seat offset) holding the bone
    hierarchy in its bind pose, and every CHR0 animation as a glTF clip (sampled per frame)."""
    # Each bone is two nodes, to get NW4R's segment scale compensation (see pose_world) out of
    # glTF, where scale always propagates: '<bone>' carries translation + rotation and holds the
    # child bones; '<bone>_scale' under it carries the bone's own scale and is the skin joint.
    nodes = g.j['nodes']
    root = len(nodes)
    nodes.append({'name': 'driver', 'translation': [float(x) for x in offset], 'children': []})
    base = len(nodes)
    for b in m.bones:
        nodes.append({'name': b.name, 'translation': list(b.trans), 'rotation': _quat(b.rot).tolist(),
                      'children': [base + len(m.bones) + b.index]})
    for b in m.bones:
        nodes.append({'name': b.name + '_scale', 'scale': list(b.scale)})
    scale_base = base + len(m.bones)
    for b in m.bones:
        if b.parent is None:
            nodes[root]['children'].append(base + b.index)
        else:
            nodes[base + b.parent.index]['children'].append(base + b.index)

    # Mesh in the bind pose; each vertex fully weighted to its bone
    mats = parse_materials(m)
    objs = {m.r.u32(o + 0x3C): o for _, o in m.sections[Mdl0.OBJS]}
    prims, joints = [], []
    for obj_i, mat_i, _ in draw_list(m):
        mesh = parse_object(m, objs[obj_i])
        if mesh.positions is None or not len(mesh.triangles):
            continue
        pos, nrm = model_space(m, mesh)
        bones = [m.matrix_bone[i] if i < len(m.matrix_bone) else -1 for i in mesh.matrix_ids]
        joints.append(np.array([max(b, 0) for b in bones]))
        tris = mesh.triangles[:, [0, 2, 1]]  # GX front faces are clockwise; glTF's are CCW
        uvs = None if mesh.uvs is None else mesh.uvs * mats[mat_i].uv_scale
        prims.append((pos, nrm, uvs, mesh.colors, tris,
                      g.material(('driver', m.name, mat_i), mats[mat_i], tex)))
    inv = np.array([np.linalg.inv(b.world).T.reshape(-1) for b in m.bones])  # column-major
    g.j.setdefault('skins', []).append({
        'joints': [scale_base + b.index for b in m.bones], 'skeleton': root,
        'inverseBindMatrices': g._accessor(inv, 'MAT4', target=None)})
    nodes.append({'name': 'driver_mesh', 'mesh': g.mesh('driver', prims, joints),
                  'skin': len(g.j['skins']) - 1})
    g.j['scenes'][0]['nodes'] += [root, len(nodes) - 1]

    # Animations. Channels that never move from the bind pose are left out.
    bind = {b.name: {'scale': np.array(b.scale), 'rot': _quat(b.rot), 'trans': np.array(b.trans)}
            for b in m.bones}
    for name, o in anims.items():
        n = chr0_frame_count(r, o)
        frames = [chr0_pose(r, o, f) for f in range(n)]
        times = g._accessor(np.arange(n, dtype=np.float32) / ANIM_FPS, 'SCALAR', target=None)
        channels, samplers = [], []
        def track(name, kind):
            out = []
            for fr in frames:
                v = fr.get(name, {}).get(kind)
                out.append(bind[name][kind] if v is None else (_quat(v) if kind == 'rot' else np.array(v)))
            return np.array(out)

        for b in m.bones:
            for kind, path in (('trans', 'translation'), ('rot', 'rotation'), ('scale', 'scale')):
                vals = track(b.name, kind)
                node = scale_base + b.index if kind == 'scale' else base + b.index
                ref = bind[b.name][kind]
                if kind == 'trans' and b.parent is not None:
                    # translation lives in the parent's scaled space (segment scale compensation)
                    vals = vals * track(b.parent.name, 'scale')
                    ref = ref * bind[b.parent.name]['scale']
                if kind == 'rot':  # keep neighbouring quaternions on the same hemisphere
                    for i in range(1, len(vals)):
                        if np.dot(vals[i], vals[i - 1]) < 0:
                            vals[i] = -vals[i]
                if np.allclose(vals, ref, atol=1e-4) or (kind == 'rot' and np.allclose(vals, -ref, atol=1e-4)):
                    continue
                samplers.append({'input': times, 'interpolation': 'LINEAR',
                                 'output': g._accessor(vals, 'VEC4' if kind == 'rot' else 'VEC3', target=None)})
                channels.append({'sampler': len(samplers) - 1,
                                 'target': {'node': node, 'path': path}})
        if channels:
            g.j.setdefault('animations', []).append({'name': name, 'channels': channels, 'samplers': samplers})

    g.j['scenes'][0]['extras'] = extras


def convert_kart(files, out_dir, disp=None, handle=None, kart_only=False):
    """Write kart.glb (one node per part) and driver.glb (rigged, with its animations).
    handle: the bike's handlebar placement from karts.json (None for karts)."""
    os.makedirs(out_dir, exist_ok=True)

    r = Reader(files['kart_model.brres'])
    c = brres_contents(r)
    tex = brres_textures(r, c)
    g = Gltf()
    body = None
    for name, o in c['3DModels(NW4R)']:
        prims = model_primitives(g, 'kart', Mdl0(r, o, name), tex)     # 'shadow' is the hull the page flattens into the ground shadow
        if name == 'body':
            offsets = np.cumsum([0] + [len(p[0]) for p in prims[:-1]])
            body = (np.vstack([p[0] for p in prims]),
                    np.vstack([p[4] + o for p, o in zip(prims, offsets)]))
        g.add_node(name, prims)
    g.save(os.path.join(out_dir, 'kart.glb'))
    if kart_only:
        return

    r = Reader(files['driver_model.brres'])
    c = brres_contents(r)
    tex = brres_textures(r, c)
    m = Mdl0(r, dict(c['3DModels(NW4R)'])['model'], 'model')
    anims = dict(c.get('AnmChr(NW4R)', []))
    # 'drive' blends by steering: frame 0 and the last frame are full lock either way, so the
    # neutral, straight-ahead pose is the middle frame.
    anim = None
    if 'drive' in anims:
        anim = chr0_pose(r, anims['drive'], (chr0_frame_count(r, anims['drive']) - 1) / 2)
    g = Gltf()
    add_rigged_driver(g, m, tex, r, anims, driver_offset(m, anim, disp, body, handle),
                      {'ik': ik_targets(disp, handle), 'fps': ANIM_FPS})
    g.save(os.path.join(out_dir, 'driver.glb'))


def convert_course(files, out_dir, textures_report=None):
    """Write track.glb (every model in course_model.brres, in game units) and sky.glb (from
    vrcorn_model.brres if present) for a course archive's files. Returns a stats dict."""
    os.makedirs(out_dir, exist_ok=True)
    stats = {}

    def export(brres_name, out_name, node_name=None, sky=False):
        if brres_name not in files:
            return
        r = Reader(files[brres_name])
        c = brres_contents(r)
        tex = brres_textures(r, c)
        srt = parse_srt0(r, c)
        g = Gltf()
        tris = 0
        for name, o in c['3DModels(NW4R)']:
            try:
                m = Mdl0(r, o, name)
                prims = model_primitives(g, brres_name, m, tex, track=True, srt_map=srt, sky=sky)
            except Exception as e:
                stats.setdefault('errors', []).append('%s/%s: %r' % (brres_name, name, e))
                continue
            tris += sum(len(p[4]) for p in prims)
            g.add_node(node_name or name, prims)
        g.save(os.path.join(out_dir, out_name))
        stats[out_name] = {'triangles': tris, 'textures': len(tex), 'bytes': os.path.getsize(os.path.join(out_dir, out_name))}

    export('course_model.brres', 'track.glb')
    export('vrcorn_model.brres', 'sky.glb', sky=True)
    return stats


# Kinoko's VEHICLE_NAMES (include/Common.hh), indexed by vehicle ID
VEHICLE_NAMES = [p + '_' + t for t in ('kart', 'bike')
                 for p in ('sdf', 'mdf', 'ldf', 'sa', 'ma', 'la', 'sb', 'mb', 'lb',
                           'sc', 'mc', 'lc', 'sd', 'md', 'ld', 'se', 'me', 'le')]


# Kinoko's Character enum order -> archive suffix (Race/Kart/<vehicle>-<code>.szs)
CHARACTER_CODES = ['mr', 'bpc', 'wl', 'kp', 'bds', 'ka', 'bmr', 'lg', 'ko', 'dk', 'ys', 'wr',
                   'blg', 'kk', 'nk', 'ds', 'pc', 'ca', 'dd', 'kt', 'jr', 'bk', 'fk', 'rs']


def driver_disp(common, vehicle, character):
    """kartDriverDispParam.bin: 36 vehicles x 48 character slots of 14 floats (see driver_pose)."""
    d = common['kartDriverDispParam.bin']
    return struct.unpack_from('>14f', d, 8 + (vehicle * 48 + character) * 56)


def load_common(mkwii):
    return u8_files(yaz0_decompress(open(os.path.join(mkwii, 'Race', 'Common.szs'), 'rb').read()))


def write_manifest(mkwii, out_dir):
    """karts.json: per bike, the handlebar pivot and rake from bikePartsDispParam.bin (18 x 44
    floats). Checked against the physics: the fork tip lands on Kinoko's front wheel centre."""
    common = load_common(mkwii)
    bikes = common['bikePartsDispParam.bin']
    manifest = {}
    for i, name in enumerate(VEHICLE_NAMES):
        if name.endswith('_bike'):
            f = struct.unpack_from('>44f', bikes, 4 + 176 * (i - 18))
            manifest[name] = {'handle': {'position': list(f[3:6]), 'rake': f[6]}}
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'karts.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    return manifest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mkwii', nargs='?', default=r'C:\Users\dwain\MKWii')
    ap.add_argument('--szs', help='convert just this archive')
    ap.add_argument('--kart-only', action='store_true', help='rewrite only kart.glb (skip the driver)')
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()

    manifest = write_manifest(args.mkwii, OUT if args.szs else args.out)
    common = load_common(args.mkwii)
    if args.szs:
        paths = [args.szs]
    else:
        kart_dir = os.path.join(args.mkwii, 'Race', 'Kart')
        # base archives only: skip _2/_4 (low detail) and _blue/_red (battle team colours)
        paths = sorted(os.path.join(kart_dir, f) for f in os.listdir(kart_dir)
                       if f.endswith('.szs') and not f.endswith(('_2.szs', '_4.szs'))
                       and '_blue' not in f and '_red' not in f and '_mii_' not in f)
    failed = []
    for n, path in enumerate(paths):
        name = os.path.basename(path)[:-4]
        out_dir = args.out if args.szs else os.path.join(args.out, name)
        vehicle, character = name.split('-')
        disp = None
        if vehicle in VEHICLE_NAMES and character in CHARACTER_CODES:
            disp = driver_disp(common, VEHICLE_NAMES.index(vehicle), CHARACTER_CODES.index(character))
        try:
            convert_kart(u8_files(yaz0_decompress(open(path, 'rb').read())), out_dir, disp,
                         manifest.get(vehicle, {}).get('handle'), args.kart_only)
        except Exception as e:  # keep going; report at the end
            failed.append((name, repr(e)))
        if n % 20 == 0:
            print('%d/%d %s' % (n, len(paths), name), flush=True)
    print('Converted %d of %d archives to %s' % (len(paths) - len(failed), len(paths),
                                                 os.path.abspath(args.out)))
    for name, err in failed:
        print('  ! %s: %s' % (name, err))


if __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == '--list':
    files = u8_files(yaz0_decompress(open(sys.argv[2], 'rb').read()))
    for path, data in files.items():
        print(path, len(data))
        if data[:4] == b'bres':
            r = Reader(data)
            for folder, entries in brres_contents(r).items():
                print('   ', folder, [n for n, _ in entries])
                if folder == '3DModels(NW4R)':
                    for n, o in entries:
                        m = Mdl0(r, o, n)
                        print('      ', n, 'matrices', len(m.matrix_bone), 'envelopes',
                              m.matrix_bone.count(-1), 'objs', len(m.sections[Mdl0.OBJS]))
                        for so_name, so in m.sections[Mdl0.OBJS]:
                            mesh = parse_object(m, so)
                            pos, _ = model_space(m, mesh)
                            print('         obj %-24s verts %5d tris %5d  min %s max %s' % (
                                so_name, len(pos), len(mesh.triangles),
                                np.round(pos.min(0), 1), np.round(pos.max(0), 1)))
                        for b in m.bones[:0]:
                            print('         %-16s parent=%-12s T=%s W=%s' % (
                                b.name, b.parent.name if b.parent else '-',
                                tuple(round(x, 1) for x in b.trans),
                                tuple(round(x, 1) for x in b.world[:3, 3])))
    sys.exit()


if __name__ == '__main__':
    main()
