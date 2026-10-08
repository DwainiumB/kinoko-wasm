"""Export the sounds the web player uses from your own revo_kart.brsar.

Reuses the BRSAR parser from extract_sounds.py, but only exports the handful of sound IDs the
page needs and records each wave's loop points (the full extractor drops the loop start), so
engine and drift loops can repeat seamlessly in Web Audio.

Usage:
    python tools/export_web_sounds.py [path/to/MKWii]      (default: C:/Users/dwain/MKWii)

Writes web/assets/sound/*.wav, web/assets/sound/sounds.json and web/assets/music/*.brstm
(course music, copied as-is; the page decodes BRSTM itself). These files are Nintendo's audio:
they are git-ignored and must not be redistributed.
"""

import csv
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'web', 'assets', 'sound')
MUSIC_OUT = os.path.join(HERE, '..', 'web', 'assets', 'music')
MKWII = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\dwain\MKWii'

sys.path.insert(0, HERE)     # extract_sounds.py (the BRSAR parser) lives next to this script
import extract_sounds as ex  # noqa: E402

# Vehicle-side effects. Keys are the names the page refers to.
SFX = {
    'drift_smoke': 409,   # SE_VCL_DRIFT_KEMURI
    'spark_blue': 410,    # SE_VCL_DRIFT_HIBANA_BLUE
    'spark_orange': 411,  # SE_VCL_DRIFT_HIBANA_RED
    'hop': 407,           # SE_VCL_MINI_JUMP
    'trick': 408,         # SE_VCL_JUMP_ACTION
    'boost': 412,         # SE_VCL_DASH
    'jump_pad': 413,      # SE_VCL_DASHJ
    'spin': 424,          # SE_VCL_SPIN
    'start_fail': 425,    # SE_VCL_START_FAIL
    'wall': 384,          # SE_VCL_COL_CONCRETE
    'brake': 393,         # SE_VCL_BRK_ASPHALT
    'slip': 339,          # SE_VCL_SLIP_ASPHALT
    'offroad': 360,       # SE_VCL_GND_DIRT
    'final_lap': 116,     # W_BGM_FINALLAP_FAN
}

# Course music by Kinoko course ID (include/Common.hh). Each has _N (laps 1-2) and _F (final lap).
COURSE_MUSIC = [
    'N_CASTLE', 'N_FARM', 'N_KINOKO', 'N_VOLCANO', 'N_FACTORY', 'N_SHOPPING', 'N_SNOWBOARD',
    'N_TRUCK', 'N_CIRCUIT1', 'N_CIRCUIT2', 'N_RIDGEHIGHWAY', 'N_TREEHOUSE', 'N_KOOPA', 'N_RAINBOW',
    'N_DESERT', 'N_WATER',
    'R_GC_BEACH', 'R_GC_CIRCUIT', 'R_GC_STADIUM', 'R_GC_MOUNTAIN', 'R_DS_JUNGLE', 'R_DS_DESERT',
    'R_DS_GARDEN', 'R_DS_TOWN', 'R_SFC_CIRCUIT', 'R_SFC_OBAKE', 'R_64_CIRCUIT', 'R_64_SHERBET',
    'R_64_KUPPA', 'R_64_JUNGLE', 'R_AGB_KUPPA', 'R_AGB_BEACH',
    'N_BTL_VENICE', 'N_BTL_BLOCK', 'N_BTL_CASINO', 'N_BTL_SKATE', 'N_BTL_SANDSTONE',
    'R_BTL_GC_COOKIE', 'R_BTL_DS_HOUSE', 'R_BTL_SFC_BTL4', 'R_BTL_AGB_BTL3', 'R_BTL_64_MATENRO',
]

# Race fanfares (streams): played before the countdown and on crossing the finish line.
JINGLES = {
    'start': 'STRM_O_START_FAN',
    'goal': 'STRM_O_T_GOAL1_FAN',
}

# Voice groups: page event -> voice action prefixes (digits and _END are variants of one line).
VOICES = {
    'boost': ['DSH'],
    'trick': ['JP_ACT'],
    'wheelie': ['WLE'],
    'jump_small': ['JP_S'],
    'jump_big': ['JP_M', 'JP_L'],
    'land': ['LND_L'],
    'hit': ['DMG_L', 'DMG_M', 'DMG_S'],
    'spin': ['DMG_SPN'],
    'start_fail': ['STR_FAIL'],
}

ENGINE_PARTS = ['UP', 'LOOP', 'STR', 'OFF', 'DOWN', 'IDLE', 'FUKASHI']


def voice_group(action):
    base = action[:-4] if action.endswith('_END') else action
    return base.rstrip('0123456789')


def wave_loop(b, rwar, idx):
    """(looped, loop_start_sample) for wave idx of an RWAR, mirroring Brsar.rwar_wave's layout."""
    tabl = rwar + b.u32(rwar + 0x10)
    datab = rwar + b.u32(rwar + 0x18)
    rv = datab + b.u32(tabl + 12 + 12 * idx + 4)
    wi = rv + b.u32(rv + 0x10) + 8
    fmt, looped = b.d[wi], b.d[wi + 1]
    start = b.u32(wi + 8)
    if fmt == 2:  # DSP-ADPCM offsets are in nibbles
        start = ex.nibbles_to_samples(start)
    return bool(looped), start


def export_sound(b, sid):
    snd = b.sounds[sid]
    typ = b.u8(snd + 0x16)
    rwar, waves = (b.wave_sound_waves(snd) if typ == 3 else b.seq_sound_waves(snd))
    files = []
    for k, w in enumerate(waves):
        res = b.rwar_wave(rwar, w)
        if not res:
            continue
        rate, chans = res
        name = '%04d.wav' % sid if len(waves) == 1 else '%04d_%d.wav' % (sid, k + 1)
        if not ex.write_wav(os.path.join(OUT, name), rate, chans):
            continue
        looped, start = wave_loop(b, rwar, w)
        entry = {'file': name}
        if looped:
            entry['loopStart'] = start / rate
            entry['loopEnd'] = len(chans[0]) / rate
        files.append(entry)
    return {'name': b.sound_name(sid), 'files': files}


def export_music(b):
    """Copy the BRSTM behind each stream sound name. Returns {sound name: file name}."""
    os.makedirs(MUSIC_OUT, exist_ok=True)
    strm_dir = os.path.join(MKWII, 'sound', 'strm')
    on_disk = {f.lower(): f for f in os.listdir(strm_dir)}
    by_name = {b.sound_name(i): i for i in range(len(b.sounds))}
    wanted = [n for base in COURSE_MUSIC for n in ('STRM_%s_N' % base, 'STRM_%s_F' % base)]
    wanted += JINGLES.values()
    files = {}
    for name in wanted:
        sid = by_name.get(name)
        if sid is None:
            print('  ! no sound named', name)
            continue
        ext = b.external_name(b.u32(b.sounds[sid] + 4)).split('/')[-1]
        src = on_disk.get(ext.lower())
        if src is None:
            print('  ! %s: %s not in sound/strm' % (name, ext))
            continue
        shutil.copyfile(os.path.join(strm_dir, src), os.path.join(MUSIC_OUT, name + '.brstm'))
        files[name] = name + '.brstm'
    return files


def main():
    os.makedirs(OUT, exist_ok=True)
    b = ex.Brsar(os.path.join(MKWII, 'sound', 'revo_kart.brsar'))

    music_files = export_music(b)
    music = {
        'courses': {
            cid: {'normal': music_files.get('STRM_%s_N' % base),
                  'final': music_files.get('STRM_%s_F' % base)}
            for cid, base in enumerate(COURSE_MUSIC)
        },
        'jingles': {k: music_files.get(v) for k, v in JINGLES.items()},
    }
    print('Copied %d music files to %s' % (len(music_files), os.path.abspath(MUSIC_OUT)))

    wanted = set(SFX.values())

    engines = {}
    with open(os.path.join(HERE, 'data', 'vehicle_engine_sounds.csv'), newline='') as f:
        for row in csv.DictReader(f):
            ids = {p: int(row[p + '_id']) for p in ENGINE_PARTS}
            engines[int(row['vehicle_id'])] = {
                'set': row['engine_set'], 'pitch': float(row['param1']),
                'maxPitch': float(row['param2']), 'ids': ids,
            }
            wanted.update(ids.values())

    voices = {}
    with open(os.path.join(HERE, 'data', 'character_voice_sounds.csv'), newline='') as f:
        for row in csv.DictReader(f):
            if row['cpu_variant'] or row['mii_pitch'] or row['prefix'] in ('DUMMY', 'TITLE'):
                continue
            group = voice_group(row['action'])
            for event, prefixes in VOICES.items():
                if group in prefixes:
                    sid = int(row['sound_id'])
                    voices.setdefault(row['character'], {}).setdefault(event, []).append(sid)
                    wanted.add(sid)

    sounds = {}
    for n, sid in enumerate(sorted(wanted)):
        sounds[sid] = export_sound(b, sid)
        if n % 100 == 0:
            print('%d/%d %s' % (n, len(wanted), sounds[sid]['name']), flush=True)

    with open(os.path.join(OUT, 'sounds.json'), 'w') as f:
        json.dump({'sfx': SFX, 'engines': engines, 'voices': voices, 'sounds': sounds,
                   'music': music}, f)

    empty = [s['name'] for s in sounds.values() if not s['files']]
    print('Exported %d sounds to %s' % (len(sounds), os.path.abspath(OUT)))
    if empty:
        print('No audio for:', ', '.join(empty))


if __name__ == '__main__':
    main()
