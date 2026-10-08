"""BRFNT (Wii bitmap font) reader: glyph sheets as PIL images plus per-character metrics."""
import struct, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import brres_to_glb as b


class _R:
    def __init__(s, d): s.d = d
    def u16(s, o): return struct.unpack('>H', s.d[o:o+2])[0]
    def u32(s, o): return struct.unpack('>I', s.d[o:o+4])[0]
    def s32(s, o): return struct.unpack('>i', s.d[o:o+4])[0]


def parse(d):
    """-> dict: cellW, cellH, baseline, sheetW, sheetH, cols, rows, sheets [PIL images], widths {glyph: (left, glyph, advance)},
    map {char code: glyph index}, plus the FINF fields (linefeed, ascent, height, ...)."""
    assert d[:4] == b'RFNT'
    off, cnt = struct.unpack('>HH', d[0x0C:0x10])
    F = {'widths': {}, 'map': {}}
    for _ in range(cnt):
        tag = d[off:off+4]; sz = struct.unpack('>I', d[off+4:off+8])[0]
        if tag == b'FINF':
            F['fontType'], F['linefeed'], F['altIndex'] = d[off+8], d[off+9], struct.unpack('>H', d[off+10:off+12])[0]
            F['defLeft'], F['defGlyph'], F['defAdvance'], F['encoding'], F['width'], F['height'], F['ascent'] = struct.unpack('>bBBBBBB', d[off+12:off+19])
        elif tag == b'TGLP':
            (F['cellW'], F['cellH'], F['baseline'], F['maxCharW'], F['sheetSize'], F['nSheets'], F['fmt'], F['cols'], F['rows'],
             F['sheetW'], F['sheetH'], dataOff) = struct.unpack('>BBbBIHHHHHHI', d[off+8:off+32])
            F['sheets'] = []
            for i in range(F['nSheets']):
                fake = bytearray(0x40) + bytearray(d[dataOff + i * F['sheetSize']: dataOff + (i + 1) * F['sheetSize']])
                struct.pack_into('>i', fake, 0x10, 0x40); struct.pack_into('>HH', fake, 0x1C, F['sheetW'], F['sheetH'])
                struct.pack_into('>I', fake, 0x20, F['fmt']); struct.pack_into('>I', fake, 0x24, 1)
                F['sheets'].append(b.decode_texture(_R(bytes(fake)), 0))
        elif tag == b'CWDH':
            first, last = struct.unpack('>HH', d[off+8:off+12])
            for i in range(first, last + 1):
                F['widths'][i] = struct.unpack('>bBB', d[off+16+3*(i-first): off+19+3*(i-first)])   # left bearing, glyph width, advance
        elif tag == b'CMAP':
            lo, hi, typ = struct.unpack('>HHH', d[off+8:off+14])
            body = off + 20
            if typ == 0:
                first = struct.unpack('>H', d[body:body+2])[0]
                for c in range(lo, hi + 1): F['map'][c] = first + (c - lo)
            elif typ == 1:
                for c in range(lo, hi + 1):
                    g = struct.unpack('>H', d[body+2*(c-lo):body+2*(c-lo)+2])[0]
                    if g != 0xFFFF: F['map'][c] = g
            elif typ == 2:
                n = struct.unpack('>H', d[body:body+2])[0]
                for i in range(n):
                    c, g = struct.unpack('>HH', d[body+2+4*i:body+6+4*i]); F['map'][c] = g
        off += sz
    return F
