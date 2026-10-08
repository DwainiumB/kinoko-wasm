"""Exports each course's enemy (CPU) route from course.kmp (ENPT points, ENPH groups) as web/assets/tracks/<folder>/enemy.json:
{points: [[x, y, z, deviation, setting1, setting2, setting3], ...], groups: [{start, len, prev: [..], next: [..]}, ...]}."""
import os, sys, json, struct
sys.path.insert(0, os.path.dirname(__file__))
import brres_to_glb as b

tracks = os.path.join(os.path.dirname(__file__), '..', 'web', 'assets', 'tracks')
for folder in sorted(os.listdir(tracks)):
    szs = os.path.join(tracks, folder, 'course.szs')
    if not os.path.exists(szs):
        continue
    files = b.u8_files(b.yaz0_decompress(open(szs, 'rb').read()))
    kmp = [v for k, v in files.items() if str(k) == 'course.kmp']
    if not kmp:
        print(folder, ': no kmp'); continue
    d = kmp[0]
    assert d[:4] == b'RKMD'
    nsec, hdr = struct.unpack('>HH', d[8:12])
    sec = {}
    for i in range(nsec):
        off = hdr + struct.unpack('>I', d[16 + 4 * i:20 + 4 * i])[0]
        sec[d[off:off + 4]] = off
    if b'ENPT' not in sec:
        print(folder, ': no ENPT'); continue
    o = sec[b'ENPT']; n = struct.unpack('>H', d[o + 4:o + 6])[0]
    pts = []
    for i in range(n):
        e = d[o + 8 + 0x14 * i: o + 8 + 0x14 * (i + 1)]
        x, y, z, dev = struct.unpack('>4f', e[:16]); s1, s2, s3 = struct.unpack('>HBB', e[16:20])
        pts.append([round(x, 1), round(y, 1), round(z, 1), round(dev, 1), s1, s2, s3])
    o = sec[b'ENPH']; m = struct.unpack('>H', d[o + 4:o + 6])[0]
    groups = []
    for i in range(m):
        e = d[o + 8 + 0x10 * i: o + 8 + 0x10 * (i + 1)]
        start, length = e[0], e[1]
        prev = [v for v in e[2:8] if v != 0xFF]; nxt = [v for v in e[8:14] if v != 0xFF]
        groups.append({'start': start, 'len': length, 'prev': prev, 'next': nxt})
    json.dump({'points': pts, 'groups': groups}, open(os.path.join(tracks, folder, 'enemy.json'), 'w'), separators=(',', ':'))
    print(folder, len(pts), 'points', len(groups), 'groups')
