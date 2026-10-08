"""Minimal BRLYT (Wii layout) reader: pane tree with placement, textures and materials."""
import struct

def u16(d, o): return struct.unpack('>H', d[o:o+2])[0]
def u32(d, o): return struct.unpack('>I', d[o:o+4])[0]
def f32(d, o): return struct.unpack('>f', d[o:o+4])[0]
def cstr(d, o):
    e = d.index(b'\0', o); return d[o:e].decode('ascii', 'replace')

def parse(d):
    assert d[:4] == b'RLYT'
    off, cnt = u16(d, 12), u16(d, 14)
    L = {'textures': [], 'materials': [], 'panes': []}
    stack = []
    for _ in range(cnt):
        tag = d[off:off+4].decode(); sz = u32(d, off + 4); b = d[off+8:off+sz]
        if tag == 'lyt1':
            L['origin'] = b[0]; L['size'] = (f32(b, 4), f32(b, 8))
        elif tag == 'txl1':
            n = u16(b, 0); base = 4
            for i in range(n):
                L['textures'].append(cstr(b, base + u32(b, 4 + i * 8)))
        elif tag == 'fnl1':
            n = u16(b, 0); L['fonts'] = [cstr(b, 4 + u32(b, 4 + i * 8)) if False else cstr(b, 4 + u32(b, 4 + 8 * i) - 0) for i in range(n)] if False else []
            base = 4
            for i in range(n):
                L['fonts'].append(cstr(b, base + u32(b, base + i * 8)))
        elif tag == 'mat1':
            n = u16(b, 0)
            for i in range(n):
                mo = u32(b, 4 + i * 4) - 8
                name = b[mo:mo+20].split(b'\0')[0].decode()
                flags = u32(b, mo + 0x3C)
                nt = (flags >> 28) & 15
                # layout after fixed head: forecolor s16*4 (8 bytes)?  keep it simple: record flags and tex refs when present
                sh = lambda o: struct.unpack('>4h', b[mo + o:mo + o + 8])
                m = {'name': name, 'flags': flags, 'fore': sh(0x14), 'back': sh(0x1C), 'reg3': sh(0x24),
                     'konst': [b[mo + 0x2C + 4 * k: mo + 0x30 + 4 * k].hex() for k in range(4)]}
                nt = flags & 3; m['maps'] = []
                for k in range(nt):
                    q = mo + 0x40 + 4 * k
                    m['maps'].append({'tex': u16(b, q), 'wrapS': b[q + 2], 'wrapT': b[q + 3]})
                L['materials'].append(m)
        elif tag in ('pan1', 'pic1', 'wnd1', 'txt1', 'bnd1'):
            p = {'type': tag, 'flags': b[0], 'origin': b[1], 'alpha': b[2], 'name': b[4:20].split(b'\0')[0].decode(),
                 'x': f32(b, 0x1C), 'y': f32(b, 0x20), 'z': f32(b, 0x24), 'rx': f32(b, 0x28), 'ry': f32(b, 0x2C), 'rz': f32(b, 0x30),
                 'sx': f32(b, 0x34), 'sy': f32(b, 0x38), 'w': f32(b, 0x3C), 'h': f32(b, 0x40), 'children': [], 'body': b}
            (stack[-1]['children'] if stack else L['panes']).append(p)
            if tag == 'txt1':
                o = 0x44
                bufBytes, strBytes, mat, font = struct.unpack('>4H', b[o:o+8])
                p.update({'mat': mat, 'font': font, 'textPos': b[o + 8], 'textAlign': b[o + 9],
                          'top': b[o + 16:o + 20].hex(), 'bottom': b[o + 20:o + 24].hex(),
                          'fontW': f32(b, o + 24), 'fontH': f32(b, o + 28), 'charSpace': f32(b, o + 32), 'lineSpace': f32(b, o + 36)})
                to = u32(b, o + 12) - 8
                p['text'] = b[to:to + strBytes].decode('utf-16-be', 'replace').rstrip(chr(0)) if to > 0 and strBytes else ''
            if tag == 'pic1':
                p['vtx'] = [b[0x44 + 4 * k: 0x48 + 4 * k].hex() for k in range(4)]
                p['mat'] = u16(b, 0x54); nc = b[0x56]
                p['uv'] = [[f32(b, 0x58 + 32 * i + 4 * j) for j in range(8)] for i in range(nc)]
            L['last'] = p
        elif tag == 'pas1':
            stack.append(L['last'])
        elif tag == 'pae1':
            stack.pop()
        off += sz
    return L

def dump(p, ind=0):
    print(' ' * ind + f"{p['type']} {p['name']} pos=({p['x']:.1f},{p['y']:.1f}) size=({p['w']:.1f},{p['h']:.1f}) scale=({p['sx']},{p['sy']}) origin={p['origin']} alpha={p['alpha']} flags={p['flags']:#x}")
    for c in p['children']: dump(c, ind + 2)


def parse_brlan(d):
    """RLAN -> {frameSize, entries: [{name, isMat, tags: [{type, items: [{target, dtype, keys}]}]}]}.
    keys are (frame, value, slope) for hermite data and (frame, value) for step data."""
    assert d[:4] == b'RLAN'
    off, cnt = u16(d, 12), u16(d, 14)
    out = {'entries': [], 'frameSize': 0}
    for _ in range(cnt):
        tag = d[off:off+4].decode(); sz = u32(d, off + 4)
        if tag == 'pai1':
            b = d[off:off+sz]
            out['frameSize'] = u16(b, 8)
            nfiles, nents = u16(b, 12), u16(b, 14)
            tbl = u32(b, 16)
            for _e in range(nents):
                eoff = u32(b, tbl + 4 * _e)
                e = {'name': b[eoff:eoff+20].split(bytes([0]))[0].decode(), 'isMat': b[eoff + 21], 'tags': []}
                for k in range(b[eoff + 20]):
                    o = eoff + u32(b, eoff + 24 + 4 * k)
                    t = {'type': b[o:o+4].decode(), 'items': []}
                    for j in range(b[o + 4]):
                        q = o + u32(b, o + 8 + 4 * j)
                        item = {'index': b[q], 'target': b[q + 1], 'dtype': b[q + 2], 'keys': []}
                        stride = 12 if item['dtype'] == 2 else 8
                        n = u16(b, q + 4)
                        base = q + u32(b, q + 8)
                        for m in range(n):
                            r = base + m * stride
                            if item['dtype'] == 2:
                                item['keys'].append((f32(b, r), f32(b, r + 4), f32(b, r + 8)))
                            else:
                                item['keys'].append((f32(b, r), u16(b, r + 4)))
                        t['items'].append(item)
                    e['tags'].append(t)
                out['entries'].append(e)
        off += sz
    return out
