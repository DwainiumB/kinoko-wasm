"""Exports the game's pause-menu art from Scene/UI/Race.szs + Race_<lang>.szs into web/assets/ui/:
   pause_menu.json     the menu button layout (button/blyt/common_w004_menu.brlyt), its animations (button/anim/*.brlan), the
                       full-screen dim layout (message_window/blyt/black_down_race.brlyt) and the menu's strings (message/Race.bmg)
   pause_pat_*.png     the two pattern textures those layouts use

Usage: python tools/export_pause_menu.py C:/path/to/Scene/UI [lang letter, default U]
"""
import os, sys, json, struct
sys.path.insert(0, os.path.dirname(__file__))
import brres_to_glb as b, tpl, brlyt

root = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\dwain\MKWii\Scene\UI'
lang = sys.argv[2] if len(sys.argv) > 2 else 'U'
out = os.path.join(os.path.dirname(__file__), '..', 'web', 'assets', 'ui')
os.makedirs(out, exist_ok=True)

files = {}
for a in ('Race', 'Race_' + lang):
    for k, v in b.u8_files(b.yaz0_decompress(open(os.path.join(root, a + '.szs'), 'rb').read())).items():
        files[str(k)] = v


def layout(path):
    L = brlyt.parse(files[path])
    pics = []
    def walk(p, parent):
        if p['type'] in ('pic1', 'pan1', 'bnd1', 'txt1'):
            e = {'name': p['name'], 'type': p['type'], 'parent': parent, 'x': p['x'], 'y': p['y'], 'w': p['w'], 'h': p['h'], 'sx': p['sx'], 'sy': p['sy'],
                 'origin': p['origin'], 'alpha': p['alpha'], 'visible': bool(p['flags'] & 1), 'inheritAlpha': bool(p['flags'] & 2)}
            if p['type'] == 'pic1':
                m = L['materials'][p['mat']]
                e.update({'vtx': p['vtx'], 'uv': p['uv'][0] if p['uv'] else None, 'fore': list(m['fore']), 'back': list(m['back']),
                          'tex': [L['textures'][t['tex']] for t in m['maps']], 'wrap': [[t['wrapS'], t['wrapT']] for t in m['maps']]})
            if p['type'] == 'txt1':
                m = L['materials'][p['mat']]
                e.update({'fore': list(m['fore']), 'back': list(m['back']), 'top': p['top'], 'bottom': p['bottom'], 'fontW': p['fontW'], 'fontH': p['fontH'],
                          'charSpace': p['charSpace'], 'textPos': p['textPos']})
            pics.append(e)
        for c in p['children']: walk(c, p['name'])
    for p in L['panes']: walk(p, None)
    return {'size': L['size'], 'panes': pics}


def anim(path):
    x = brlyt.parse_brlan(files[path])
    return {'frames': x['frameSize'], 'entries': [{'name': e['name'], 'mat': e['isMat'], 'tags': [
        {'type': t['type'], 'items': [{'target': i['target'], 'dtype': i['dtype'], 'keys': i['keys']} for i in t['items']]} for t in e['tags']]} for e in x['entries']]}


def bmg(d):
    """message/Race.bmg: id -> text (utf-16)"""
    assert d[:8] == b'MESGbmg1'
    n = struct.unpack('>I', d[12:16])[0]
    o = 32; sec = {}
    for _ in range(n):
        tag = d[o:o + 4]; sz = struct.unpack('>I', d[o + 4:o + 8])[0]; sec[tag] = d[o + 8:o + sz]; o += sz
    inf, dat, mid = sec[b'INF1'], sec[b'DAT1'], sec.get(b'MID1')
    cnt, esz = struct.unpack('>HH', inf[:4])
    ids = struct.unpack('>%dI' % cnt, mid[8:8 + 4 * cnt]) if mid else range(cnt)
    res = {}
    for i in range(cnt):
        so = struct.unpack('>I', inf[8 + i * esz: 12 + i * esz])[0]
        e = so
        while dat[e:e + 2] != b'\0\0': e += 2
        res[ids[i]] = dat[so:e].decode('utf-16-be', 'replace')
    return res

msgs = bmg(files['message/Race.bmg'])
wanted = ['Continue', 'Restart', 'Change Course', 'Change Character', 'Change Ghost', 'Quit', 'Yes', 'No', 'Pause', 'Resume', 'Change Kart']
strings = {}
for i, t in msgs.items():
    s = t.strip()
    if any(s.startswith(w) for w in wanted) and len(s) < 40: strings[str(i)] = s

J = {'button': layout('button/blyt/common_w004_menu.brlyt'),
     'dim': layout('message_window/blyt/black_down_race.brlyt'),
     'anims': {n: anim('button/anim/common_w004_menu_%s.brlan' % n) for n in ('fade_in', 'fade_out', 'free', 'free_to_select', 'select', 'select_to_free', 'text_light_01_ok', 'text_light_01_stop')},
     'strings': strings}
for k in files:
    if k.startswith('message_window/anim/') and 'black_down' in k: J['anims'][os.path.basename(k)[:-6]] = anim(k)
# the labels use the kart font, which has only capitals and digits; the game falls back to kart_kanji_font for the other glyphs
import brfnt
fnt = {str(k): v for k, v in b.u8_files(b.yaz0_decompress(open(os.path.join(root, 'Font.szs'), 'rb').read())).items()}
kf = brfnt.parse(fnt['kart_kanji_font.brfnt'])
for i, im in enumerate(kf['sheets']): im.convert('RGBA').save(os.path.join(out, 'pause_font_%d.png' % i))
J['font'] = {k: kf[k] for k in ('cellW', 'cellH', 'baseline', 'linefeed', 'cols', 'rows', 'sheetW', 'sheetH', 'fmt', 'defGlyph')}
J['font']['widths'] = {str(g): list(w) for g, w in kf['widths'].items()}
J['font']['map'] = {str(c): g for c, g in kf['map'].items() if c < 0x250}
J['font']['sheetCount'] = len(kf['sheets'])
json.dump(J, open(os.path.join(out, 'pause_menu.json'), 'w'), separators=(',', ':'))
for src, name in (('button/timg/tt_pattern_chek_bokashi_32x32.tpl', 'pause_pat_bokashi'), ('message_window/timg/tt_pattern_chek_32x32.tpl', 'pause_pat_chek')):
    im = tpl.decode(files[src]).convert('RGBA'); im.save(os.path.join(out, name + '.png')); print(name, im.size)
print('pause_menu.json', os.path.getsize(os.path.join(out, 'pause_menu.json')), 'bytes;', len(strings), 'strings')
for i, s in sorted(strings.items(), key=lambda x: int(x[0])): print(' ', i, s)
