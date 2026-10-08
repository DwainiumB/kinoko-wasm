"""Exports each course's minimap (map_model.brres, a flat white mesh seen from above) as web/assets/tracks/<folder>/minimap.json:
{bbox: [minX, minZ, maxX, maxZ], v: [x, z, x, z, ...] (whole units), t: [i, j, k, ...]}."""
import os, sys, json
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import brres_to_glb as b

tracks = os.path.join(os.path.dirname(__file__), '..', 'web', 'assets', 'tracks')
for folder in sorted(os.listdir(tracks)):
    szs = os.path.join(tracks, folder, 'course.szs')
    if not os.path.exists(szs):
        continue
    files = b.u8_files(b.yaz0_decompress(open(szs, 'rb').read()))
    m = [v for k, v in files.items() if str(k) == 'map_model.brres']
    if not m:
        print(folder, ': no map_model'); continue
    r = b.Reader(m[0]); c = b.brres_contents(r)
    mdls = c.get('3DModels(NW4R)', [])
    if not mdls:
        print(folder, ': empty'); continue
    name, o = mdls[0]; mdl = b.Mdl0(r, o, name)
    objs = {i: oo for i, oo in ((mdl.r.u32(oo + 0x3C), oo) for _, oo in mdl.sections[b.Mdl0.OBJS])}
    P, T, off = [], [], 0
    for oi, mi, _ in b.draw_list(mdl):
        ms = b.parse_object(mdl, objs[oi])
        if ms.positions is None or not len(ms.triangles):
            continue
        pos, _n = b.model_space(mdl, ms, None)
        P.append(pos[:, [0, 2]]); T.append(ms.triangles + off); off += len(pos)
    P = np.vstack(P); T = np.vstack(T)
    used = np.unique(T); remap = -np.ones(len(P), int); remap[used] = np.arange(len(used))
    P = P[used]; T = remap[T]
    # the map's viewport: the model's posLD (left-down) and posRU (right-up) bones mark the corners of the area the game shows
    bones = {bn.name: bn for bn in mdl.bones}
    if 'posLD' in bones and 'posRU' in bones:
        ld, ru = bones['posLD'].trans, bones['posRU'].trans
        corners = [float(ld[0]), float(ru[2]), float(ru[0]), float(ld[2])]      # minX, minZ, maxX, maxZ
    else:
        corners = [float(P[:, 0].min()), float(P[:, 1].min()), float(P[:, 0].max()), float(P[:, 1].max())]
    out = {'corners': corners, 'bbox': [float(P[:, 0].min()), float(P[:, 1].min()), float(P[:, 0].max()), float(P[:, 1].max())],
           'v': np.round(P).astype(int).ravel().tolist(), 't': T.ravel().tolist()}
    with open(os.path.join(tracks, folder, 'minimap.json'), 'w') as f:
        json.dump(out, f, separators=(',', ':'))
    print(folder, len(P), 'verts', len(T), 'tris', 'span', np.round(P.max(0) - P.min(0)).astype(int).tolist())
