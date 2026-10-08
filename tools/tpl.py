"""Nintendo TPL texture -> PIL image, reusing the GX decoders of brres_to_glb."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import brres_to_glb as b

class _R:
    def __init__(s, d): s.d = d
    def u16(s, o): return struct.unpack('>H', s.d[o:o+2])[0]
    def u32(s, o): return struct.unpack('>I', s.d[o:o+4])[0]
    def s32(s, o): return struct.unpack('>i', s.d[o:o+4])[0]

def decode(t):
    assert struct.unpack('>I', t[:4])[0] == 0x0020AF30, 'not a TPL'
    ih, ph = struct.unpack('>II', t[12:20])
    h, w, fmt, do = struct.unpack('>HHII', t[ih:ih+12])
    fake = bytearray(0x40) + bytearray(t[do:])
    struct.pack_into('>i', fake, 0x10, 0x40); struct.pack_into('>HH', fake, 0x1C, w, h)
    struct.pack_into('>I', fake, 0x20, fmt); struct.pack_into('>I', fake, 0x24, 1)
    im = b.decode_texture(_R(bytes(fake)), 0)
    im.info['fmt'] = fmt
    return im
