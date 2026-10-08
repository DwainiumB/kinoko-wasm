"""Exports the race HUD's art from the game's UI archives (Scene/UI/Race.szs + Race_<lang>.szs) as PNGs in web/assets/ui/.

Usage: python tools/export_hud.py C:/path/to/Scene/UI [lang letter, default U]
The textures are the game's own (TPL, decoded with the GX decoders of brres_to_glb.py); colouring happens in the page.
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import brres_to_glb as b, tpl

root = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\dwain\MKWii\Scene\UI'
lang = sys.argv[2] if len(sys.argv) > 2 else 'U'
out = os.path.join(os.path.dirname(__file__), '..', 'web', 'assets', 'ui')
os.makedirs(out, exist_ok=True)

def arc(n):
    return b.u8_files(b.yaz0_decompress(open(os.path.join(root, n + '.szs'), 'rb').read()))

files = {}
for a in ('Race', 'Race_' + lang):
    for k, v in arc(a).items():
        files[str(k)] = v

names = ['tt_item_box_glass_type_02', 'tt_item_kinoko', 'tt_item_kinoko_2', 'tt_item_kinoko_3',
         'tt_d_number_3d_slash', 'tt_d_number_3d_coron', 'tt_d_number_3d_coron_00', 'tt_time_E', 'tt_lap_E']
names += ['tt_d_number_3d_%02d' % i for i in range(10)]
names += ['tt_position_no_st_64x64_%02d' % i for i in range(1, 13)]      # the VS race position (1st ... 12th)
# the map's racer icons (one per character, in the game's character order) and the heading searchlight
names += ['tt_map_chara_searchlight'] + ['st_%s_32x32' % n for n in
          ['mario', 'baby_peach', 'waluigi', 'koopa', 'baby_daisy', 'karon', 'baby_mario', 'luigi', 'kinopio', 'donky', 'yoshi', 'wario',
           'baby_luigi', 'kinopico', 'noko', 'daisy', 'peach', 'catherine', 'didy', 'teresa', 'koopa_jr', 'hone_koopa', 'fuky', 'roseta']]
for n in names:
    t = files.get('game_image/timg/%s.tpl' % n)
    if t is None:
        print('missing', n); continue
    im = tpl.decode(t).convert('RGBA')
    im.save(os.path.join(out, n + '.png'))
    print(n, im.size)


# ---- the race messages: countdown / GO! / FINISH! / NEW RECORD! (game_image/blyt/go.brlyt, new_record.brlyt + their animations) and the font ----
import json, brfnt, brlyt

def font_export():
    fnt = {str(k): v for k, v in b.u8_files(b.yaz0_decompress(open(os.path.join(root, 'Font.szs'), 'rb').read())).items()}
    f = brfnt.parse(fnt['tt_kart_font_rodan_ntlg_pro_b.brfnt'])
    for i, im in enumerate(f['sheets']):
        im.convert('RGBA').save(os.path.join(out, 'font_kart_%d.png' % i))
    meta = {k: f[k] for k in ('cellW', 'cellH', 'baseline', 'linefeed', 'cols', 'rows', 'sheetW', 'sheetH', 'fmt', 'defGlyph')}
    meta['widths'] = {str(g): list(w) for g, w in f['widths'].items()}
    meta['map'] = {str(c): g for c, g in f['map'].items()}
    return meta

def layout_export(name, anims):
    L = brlyt.parse(files['game_image/blyt/%s.brlyt' % name])
    panes = []
    def walk(p, parent=None):
        if p['type'] == 'txt1':
            m = L['materials'][p['mat']]
            panes.append({'name': p['name'], 'x': p['x'], 'y': p['y'], 'w': p['w'], 'h': p['h'], 'sx': p['sx'], 'sy': p['sy'], 'alpha': p['alpha'],
                          'top': p['top'], 'bottom': p['bottom'], 'fontW': p['fontW'], 'fontH': p['fontH'], 'charSpace': p['charSpace'],
                          'lineSpace': p['lineSpace'], 'textPos': p['textPos'], 'fore': list(m['fore']), 'back': list(m['back']), 'parent': parent})
        for c in p['children']: walk(c, p['name'])
    for p in L['panes']: walk(p)
    an = {}
    for a in anims:
        x = brlyt.parse_brlan(files['game_image/anim/%s.brlan' % a])
        an[a] = {'frames': x['frameSize'], 'entries': [{'name': e['name'], 'mat': e['isMat'], 'tags': [
            {'type': t['type'], 'items': [{'target': i['target'], 'dtype': i['dtype'], 'keys': i['keys']} for i in t['items']]} for t in e['tags']]} for e in x['entries']]}
    return {'panes': panes, 'anims': an}

msgs = {'font': font_export(),
        'go': layout_export('go', ['go_fade_in_before', 'go_fade_in']),
        'new_record': layout_export('new_record', ['new_record_fade_in_before', 'new_record_fade_in'])}
json.dump(msgs, open(os.path.join(out, 'hud_messages.json'), 'w'), separators=(',', ':'))
print('hud_messages.json', os.path.getsize(os.path.join(out, 'hud_messages.json')), 'bytes')
