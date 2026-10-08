"""Extract every sound from Mario Kart Wii (revo_kart.brsar + strm/*.brstm)
into WAV files, grouped into folders by what they are.

Usage:
    python extract_sounds.py                 # everything
    python extract_sounds.py --no-music      # skip the .brstm music (slowest part)
    python extract_sounds.py --only Vehicles # only top-level folders starting with this

Output goes to "extracted sounds/All sounds/" next to this script, plus
index.csv mapping every sound ID to the file(s) it produced.

Sound types in the archive:
  * WAVE sounds (voices, most effects) -> exported as recorded.
  * SEQ sounds (engines, many effects) are little MIDI-style sequences that
    play instrument samples. For those we export the samples the sequence
    uses. The game pitches, loops and layers them at runtime, so they can
    sound shorter or lower/higher than in-game.
  * STRM sounds (music) are the separate strm/*.brstm files.

Pure Python 3, no extra packages.
"""
import argparse
import csv
import os
import re
import struct
import sys
import time
import wave
from array import array

HERE = os.path.dirname(os.path.abspath(__file__))
BRSAR = os.path.join(HERE, 'sound', 'revo_kart.brsar')
STRM_DIR = os.path.join(HERE, 'sound', 'strm')
OUT = os.path.join(HERE, 'extracted sounds', 'All sounds')
MAX_GUESSED = 24  # skip runtime-chosen-instrument sounds with more candidates than this

# --------------------------------------------------------------------------
# Naming / grouping
# --------------------------------------------------------------------------

CHARS = {'MR': 'Mario', 'LG': 'Luigi', 'YS': 'Yoshi', 'PC': 'Peach', 'DS': 'Daisy', 'CA': 'Birdo',
         'JR': 'Bowser Jr', 'DD': 'Diddy Kong', 'BMR': 'Baby Mario', 'BLG': 'Baby Luigi',
         'BPC': 'Baby Peach', 'BDS': 'Baby Daisy', 'KK': 'Toadette', 'KO': 'Toad', 'KA': 'Dry Bones',
         'NK': 'Koopa Troopa', 'WR': 'Wario', 'WL': 'Waluigi', 'KP': 'Bowser', 'DK': 'Donkey Kong',
         'KT': 'King Boo', 'FK': 'Funky Kong', 'BK': 'Dry Bowser', 'RS': 'Rosalina'}

TRACKS = {
    'LGC': 'Luigi Circuit', 'MMC': 'Moo Moo Meadows', 'KNK': 'Mushroom Gorge', 'FCT': "Toad's Factory",
    'MRC': 'Mario Circuit', 'SHP': 'Coconut Mall', 'BDX': 'DK Summit', 'TRK': "Wario's Gold Mine",
    'DSC': 'Daisy Circuit', 'WTR': 'Koopa Cape', 'SBK': 'Dry Dry Ruins', 'VLC': 'Grumble Volcano',
    'TGE': 'Moonview Highway', 'TRE': 'Maple Treeway', 'KPC': "Bowser's Castle", 'RBW': 'Rainbow Road',
    'GBH': 'GBA Shy Guy Beach', 'GBK': 'GBA Bowser Castle 3', 'SFO': 'SNES Ghost Valley 2',
    'SFM': 'SNES Mario Circuit 3', '64S': 'N64 Sherbet Land', '64M': 'N64 Mario Raceway',
    '64D': "N64 DK's Jungle Parkway", '64K': "N64 Bowser's Castle", 'GCP': 'GCN Peach Beach',
    'GCM': 'GCN Mario Circuit', 'GCW': 'GCN Waluigi Stadium', 'GCD': 'GCN DK Mountain',
    'DSY': 'DS Yoshi Falls', 'DSS': 'DS Desert Hills', 'DSM': 'DS Delfino Square', 'DSP': 'DS Peach Gardens',
    'BRK': 'Block Plaza', 'VNC': 'Delfino Pier', 'SKT': 'Funky Stadium', 'RYS': 'Thwomp Desert',
    'CSN': 'Chain Chomp Wheel', 'SFC': 'SNES Battle Course 4', 'GBA': 'GBA Battle Course 3',
    'N64': 'N64 Skyscraper', 'NGC': 'GCN Cookie Land', 'NDS': 'DS Twilight House',
    'BRM': 'Unknown arena (CM_BRM)'}

MUSIC = {
    'CIRCUIT1': 'Luigi Circuit', 'FARM': 'Moo Moo Meadows', 'KINOKO': 'Mushroom Gorge',
    'FACTORY': "Toad's Factory", 'CASTLE': 'Mario Circuit', 'SHOPPING': 'Coconut Mall',
    'SNOWBOARD': 'DK Summit', 'TRUCK': "Wario's Gold Mine", 'WATER': 'Koopa Cape',
    'CIRCUIT2': 'Daisy Circuit', 'DESERT': 'Dry Dry Ruins', 'VOLCANO': 'Grumble Volcano',
    'RIDGEHIGHWAY': 'Moonview Highway', 'TREEHOUSE': 'Maple Treeway', 'KOOPA': "Bowser's Castle",
    'RAINBOW': 'Rainbow Road', 'AGB_BEACH': 'GBA Shy Guy Beach', 'SFC_OBAKE': 'SNES Ghost Valley 2',
    'SFC_CIRCUIT': 'SNES Mario Circuit 3', 'AGB_KUPPA': 'GBA Bowser Castle 3',
    '64_SHERBET': 'N64 Sherbet Land', '64_CIRCUIT': 'N64 Mario Raceway',
    '64_JUNGLE': "N64 DK's Jungle Parkway", '64_KUPPA': "N64 Bowser's Castle",
    'GC_BEACH': 'GCN Peach Beach', 'GC_CIRCUIT': 'GCN Mario Circuit', 'GC_STADIUM': 'GCN Waluigi Stadium',
    'GC_MOUNTAIN': 'GCN DK Mountain', 'DS_JUNGLE': 'DS Yoshi Falls', 'DS_TOWN': 'DS Delfino Square',
    'DS_DESERT': 'DS Desert Hills', 'DS_GARDEN': 'DS Peach Gardens',
    'BTL_VENICE': 'Delfino Pier', 'BTL_BLOCK': 'Block Plaza', 'BTL_SKATE': 'Funky Stadium',
    'BTL_CASINO': 'Chain Chomp Wheel', 'BTL_SANDSTONE': 'Thwomp Desert', 'BTL_GC_COOKIE': 'GCN Cookie Land',
    'BTL_DS_HOUSE': 'DS Twilight House', 'BTL_SFC_BTL4': 'SNES Battle Course 4',
    'BTL_AGB_BTL3': 'GBA Battle Course 3', 'BTL_64_MATENRO': 'N64 Skyscraper'}

ENGINES = {
    'CAR_BASIC': 'CAR_BASIC (Classic Dragster, Offroader, Cheep Charger, Super Blooper, Piranha Prowler, '
                 'Tiny Titan, Daytripper, Jetsetter, Blue Falcon, Sprinter, Honeycoupe, Magikruiser, '
                 'Sneakster, Spear, Jet Bubble, Dolphin Dasher, Phantom)',
    'CAR_KART': 'CAR_KART (Standard Kart S-M-L, Booster Seat)',
    'CAR_V8': 'CAR_V8 (Mini Beast, Wild Wing, Flame Flyer)',
    'BIKE_2ST': 'BIKE_2ST (Bit Bike, Sugarscoot)',
    'BIKE_4ST': 'BIKE_4ST (Standard Bike S-M-L, Mach Bike, Flame Runner, Wario Bike, Zip Zip)',
    'BIKE_TYPEC': 'BIKE_TYPEC (Bullet Bike, Quacker, Shooting Star)'}

VOICE_ACTIONS = [  # (prefix of action, folder) - checked in order
    ('SELECT', 'Menu selection'), ('DSH', 'Boost'), ('WLE_FAIL', 'Wheelie fail'), ('WLE', 'Wheelie'),
    ('JP_ACT', 'Trick'), ('JP', 'Jump'), ('LND', 'Landing'), ('DMG', 'Hit'), ('FAL', 'Fall off course'),
    ('RET', 'Lakitu rescue'), ('ITM_TRW', 'Item throw'), ('ITM_PUT', 'Item drop'),
    ('ITM_SCES', 'Item hit someone'), ('ITM_PWUP', 'Power-up'), ('ITM', 'Item'), ('OVTAK', 'Overtake'),
    ('GOL', 'Finish'), ('STR_FAIL', 'Bad start'), ('STRM', 'Other (STRM)'), ('CAN', 'Cannon'),
    ('ATK', 'Attack'), ('TICO', 'Luma'), ('SELECT_KART', 'Menu selection')]


def voice_action(rest):
    npc = rest.startswith('NPC_')
    if npc:
        rest = rest[4:]
    folder = 'Other'
    for pre, name in VOICE_ACTIONS:
        if rest.startswith(pre):
            folder = name
            break
    return folder + (' (CPU)' if npc else '')


def group_for(name):
    """Return the folder path (list of parts) for a sound name."""
    p = name.split('_')
    if name.startswith('VO_'):
        code = p[1]
        rest = '_'.join(p[2:])
        if code in CHARS:
            return ['Characters', CHARS[code], voice_action(rest)]
        if re.fullmatch(r'[MF][1-4]', code):
            gender = 'Male' if code[0] == 'M' else 'Female'
            pitch = {'N': 'normal pitch', 'H': 'high pitch', 'L': 'low pitch'}.get(rest[:1], 'other')
            return ['Characters', 'Mii', '%s voice %s' % (gender, code[1]), pitch, voice_action(rest[2:])]
        if code == 'TITLE':
            return ['Characters', 'Title screen voices']
        return ['Characters', 'Other voices']
    if name.startswith('SE_VCL_'):
        for k, v in ENGINES.items():
            if name.startswith('SE_VCL_' + k + '_'):
                return ['Vehicles', 'Engines', v]
        sub = {'SLIP': 'Tyre slip', 'GND': 'Driving surface', 'RUN': 'Rolling surface', 'BRK': 'Braking',
               'COL': 'Collisions', 'DRIFT': 'Drift'}.get(p[2], 'Actions')
        return ['Vehicles', sub]
    if name.startswith('SE_CN_'):
        return ['Courses', 'Nitro', TRACKS.get(p[2], p[2])]
    if name.startswith('SE_CR_'):
        return ['Courses', 'Retro', TRACKS.get(p[2], p[2])]
    if name.startswith(('SE_CB_', 'SE_CM_')):
        return ['Courses', 'Battle arenas', TRACKS.get(p[2], p[2])]
    simple = {'SE_CC': ['Courses', 'Shared course objects'], 'SE_ITM': ['Items'],
              'SE_RC': ['Race system'], 'SE_JUGEM': ['Lakitu'], 'SE_UI': ['Menus'],
              'SE_RSLT': ['Results screen'], 'SE_DEMO': ['Cutscenes']}
    k = '_'.join(p[:2])
    if k in simple:
        return simple[k]
    if name.startswith('STRM_') and len(p) >= 4 and p[1] in ('N', 'R') and p[-1] in ('N', 'F'):
        body = '_'.join(p[2:-1])
        kind = 'Battle' if body.startswith('BTL') else ('Nitro' if p[1] == 'N' else 'Retro')
        return ['Music', kind + ' courses', MUSIC.get(body, body)]
    if name.startswith(('STRM_', 'SEQ_', 'W_BGM')):
        return ['Music', 'Menus and jingles']
    return ['Other']


def safe(s):
    return re.sub(r'[<>:"/\\|?*]', '-', s).strip().rstrip('.')


# --------------------------------------------------------------------------
# Audio decoding / WAV writing
# --------------------------------------------------------------------------

def decode_adpcm(buf, off, nsamples, coefs, h1, h2):
    """Nintendo DSP-ADPCM -> list of int16. 8-byte frames, 14 samples each."""
    out = array('h', bytes(2 * nsamples))
    c = coefs
    i = 0
    while i < nsamples:
        ps = buf[off]
        scale = 1 << (ps & 0xF) << 11
        ci = (ps >> 4) * 2
        c1 = c[ci] if ci < 16 else 0
        c2 = c[ci + 1] if ci < 16 else 0
        for b in buf[off + 1:off + 8]:
            for nib in (b >> 4, b & 0xF):
                if i >= nsamples:
                    break
                if nib >= 8:
                    nib -= 16
                s = (nib * scale + 1024 + c1 * h1 + c2 * h2) >> 11
                if s > 32767:
                    s = 32767
                elif s < -32768:
                    s = -32768
                out[i] = s
                h2 = h1
                h1 = s
                i += 1
        off += 8
    return out, h1, h2


def nibbles_to_samples(n):
    return (n // 16) * 14 + max(0, n % 16 - 2)


def write_wav(path, rate, channels):
    """channels: list of array('h') (same length)."""
    n = min(len(c) for c in channels)
    if n == 0:
        return False
    nch = len(channels)
    if nch == 1:
        inter = channels[0][:n]
    else:
        inter = array('h', bytes(2 * n * nch))
        for ci, c in enumerate(channels):
            inter[ci::nch] = c[:n]
    if sys.byteorder == 'big':
        inter.byteswap()
    if os.path.dirname(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, 'wb') as w:
        w.setnchannels(nch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(inter.tobytes())
    return True


# --------------------------------------------------------------------------
# BRSAR parsing
# --------------------------------------------------------------------------

class Brsar:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        if d[:4] != b'RSAR':
            raise SystemExit('Not a BRSAR: ' + path)
        symb, info = self.u32(0x10), self.u32(0x18)
        sb = symb + 8
        st = sb + self.u32(sb)
        self.strings = []
        for i in range(self.u32(st)):
            o = sb + self.u32(st + 4 + 4 * i)
            self.strings.append(d[o:d.index(b'\0', o)].decode('ascii', 'replace'))
        self.ib = info + 8
        self.sounds = self.table(self.ref(self.ib + 0))
        self.banks = self.table(self.ref(self.ib + 8))
        self.files = self.table(self.ref(self.ib + 24))
        self.groups = self.table(self.ref(self.ib + 32))
        self._rwar_cache = {}
        self.last_seq_guessed = False

    def u8(self, o): return self.d[o]
    def u32(self, o): return struct.unpack_from('>I', self.d, o)[0]
    def s32(self, o): return struct.unpack_from('>i', self.d, o)[0]
    def ref(self, o): return self.ib + self.u32(o + 4)

    def table(self, o):
        return [self.ref(o + 4 + 8 * i) for i in range(self.u32(o))]

    def sound_name(self, i):
        sid = self.u32(self.sounds[i])
        return self.strings[sid] if sid < len(self.strings) else 'SOUND_%d' % i

    def file_loc(self, fid):
        """(main_offset, main_size, wave_offset, wave_size) of an internal file, or None."""
        fo = self.files[fid]
        best = None
        for r in self.table(self.ref(fo + 0x14)):
            gid, idx = self.u32(r), self.u32(r + 4)
            g = self.groups[gid]
            it = self.table(self.ref(g + 0x20))[idx]
            loc = (self.u32(g + 0x10) + self.u32(it + 4), self.u32(it + 8),
                   self.u32(g + 0x18) + self.u32(it + 0xC), self.u32(it + 0x10))
            if best is None or (loc[3] and not best[3]):
                best = loc
        return best

    def external_name(self, fid):
        """External file path of a file entry (used by streams), or ''."""
        o = self.u32(self.files[fid] + 0xC + 4)
        if not o:
            return ''
        p = self.ib + o
        return self.d[p:self.d.index(b'\0', p)].decode('ascii', 'replace')

    # ---- RWAR / RWAV -----------------------------------------------------
    def rwar_wave(self, rwar, idx):
        """Decode wave idx of the RWAR at offset rwar -> (rate, [channels])."""
        key = (rwar, idx)
        if key in self._rwar_cache:
            return self._rwar_cache[key]
        d = self.d
        tabl = rwar + self.u32(rwar + 0x10)
        datab = rwar + self.u32(rwar + 0x18)
        if idx >= self.u32(tabl + 8):
            return None
        rv = datab + self.u32(tabl + 12 + 12 * idx + 4)
        if d[rv:rv + 4] != b'RWAV':
            return None
        wi = rv + self.u32(rv + 0x10) + 8          # WaveInfo
        wdata = rv + self.u32(rv + 0x18) + 8        # sample data
        fmt, _loop, nch = d[wi], d[wi + 1], d[wi + 2]
        rate = (d[wi + 3] << 16) | struct.unpack_from('>H', d, wi + 4)[0]
        loop_end = self.u32(wi + 0xC)
        ctab = wi + self.u32(wi + 0x10)
        chans = []
        for c in range(nch):
            ci = wi + self.u32(ctab + 4 * c)
            doff = wdata + self.u32(ci)
            if fmt == 2:
                ai = wi + self.u32(ci + 4)
                coefs = struct.unpack_from('>16h', d, ai)
                h1, h2 = struct.unpack_from('>hh', d, ai + 0x24)
                n = nibbles_to_samples(loop_end + 1)
                chans.append(decode_adpcm(d, doff, n, coefs, h1, h2)[0])
            elif fmt == 1:
                a = array('h', d[doff:doff + 2 * (loop_end + 1)])
                if sys.byteorder == 'little':
                    a.byteswap()
                chans.append(a)
            else:
                chans.append(array('h', [((b ^ 0x80) - 128) << 8 for b in d[doff:doff + loop_end + 1]]))
        res = (rate, chans)
        self._rwar_cache = {key: res}  # keep only the last one (waves reused back-to-back)
        return res

    # ---- WAVE sounds (RWSD) ---------------------------------------------
    def wave_sound_waves(self, snd):
        """Return (rwar_offset, [wave indices]) for a WAVE-type sound."""
        fid = self.u32(snd + 4)
        loc = self.file_loc(fid)
        if not loc or not loc[3]:
            return None, []
        mo, _, wo, _ = loc
        sub = self.u32(self.ref(snd + 0x18))
        db = mo + self.u32(mo + 0x10) + 8
        if sub >= self.u32(db):
            return wo, []
        e = db + self.u32(db + 4 + 8 * sub + 4)
        nt = db + self.u32(e + 0x14)
        waves = []
        for i in range(self.u32(nt)):
            ni = db + self.u32(nt + 4 + 8 * i + 4)
            w = self.s32(ni)
            if w >= 0 and w not in waves:
                waves.append(w)
        return wo, waves

    # ---- SEQ sounds (RSEQ + RBNK) ---------------------------------------
    def seq_notes(self, seq_start, label):
        """Walk a sequence from label (following every branch), return the set
        of (program, key) it can play. program None = chosen at runtime."""
        d = self.d
        notes = set()
        # (offset, program, transpose, known sequence variables)
        todo = [(label, 0, 0, frozenset())]
        seen = set()
        steps = 0
        MAX_STEPS = 50000

        while todo and steps < MAX_STEPS:
            o, prg, tr, fv = todo.pop()
            vars_ = dict(fv)
            while steps < MAX_STEPS:
                steps += 1
                fv = frozenset(vars_.items())
                if (o, prg, tr, fv) in seen:
                    break
                seen.add((o, prg, tr, fv))
                try:
                    # Prefixes: A2 = "if" (command is conditional), A0 = random,
                    # A1 = variable (both replace the last argument),
                    # A3/A4/A5 = time prefixes (append an extra argument).
                    cond, pre = False, None
                    while True:
                        cmd = d[seq_start + o]
                        o += 1
                        if cmd == 0xA2:
                            cond = True
                        elif cmd in (0xA0, 0xA1, 0xA3, 0xA4, 0xA5):
                            pre = cmd
                        else:
                            break

                    def last(kind):
                        """Read the command's last argument, honouring A0/A1."""
                        nonlocal o
                        if pre == 0xA0:
                            lo, hi = struct.unpack_from('>hh', d, seq_start + o)
                            o += 4
                            return ('rand', lo, hi)
                        if pre == 0xA1:
                            idx = d[seq_start + o]
                            o += 1
                            return vars_[idx] if idx in vars_ else ('var',)
                        if kind == 'var':
                            v = 0
                            while True:
                                b = d[seq_start + o]
                                o += 1
                                v = (v << 7) | (b & 0x7F)
                                if not b & 0x80:
                                    return v
                        if kind == 's8':
                            v = d[seq_start + o]
                            o += 1
                            return v - 256 if v > 127 else v
                        if kind == 's16':
                            v = struct.unpack_from('>h', d, seq_start + o)[0]
                            o += 2
                            return v
                        raise ValueError(kind)

                    stop = False
                    if cmd < 0x80:                          # note: key, velocity, length
                        notes.add((prg, None if tr is None else max(0, min(127, cmd + tr))))
                        o += 1
                        last('var')
                    elif cmd == 0x80:                       # wait
                        last('var')
                    elif cmd == 0x81:                       # program change
                        v = last('var')
                        if isinstance(v, int):
                            prg = v
                        elif v[0] == 'rand':                # random program: try each
                            for p in range(max(0, v[1]), min(v[2], 255) + 1):
                                todo.append((o, p, tr, frozenset(vars_.items())))
                            stop = True
                        else:
                            prg = None
                    elif cmd == 0x88:                       # open track: u8 track, u24 offset
                        tgt = int.from_bytes(d[seq_start + o + 1:seq_start + o + 4], 'big')
                        o += 4
                        todo.append((tgt, prg, tr, frozenset(vars_.items())))
                    elif cmd in (0x89, 0x8A):               # jump / call
                        tgt = int.from_bytes(d[seq_start + o:seq_start + o + 3], 'big')
                        o += 3
                        if cmd == 0x8A or cond:
                            # continuation / not-taken branch
                            todo.append((o, prg, tr, frozenset(vars_.items())))
                        o = tgt
                    elif 0xB0 <= cmd <= 0xDF:               # 1-byte parameter commands
                        v = last('s8')
                        if cmd == 0xC3:                     # transpose (None = set at runtime)
                            tr = v if isinstance(v, int) else None
                    elif 0xE0 <= cmd <= 0xE3:
                        last('s16')
                    elif cmd == 0xF0:                       # extended: variables / compares
                        sub = d[seq_start + o]
                        o += 1
                        if 0x80 <= sub <= 0x8B or 0x90 <= sub <= 0x95:
                            idx = d[seq_start + o]
                            o += 1
                            v = last('s16')
                            if sub == 0x80 and isinstance(v, int):
                                if cond:                    # conditional: fork with and without
                                    todo.append((o, prg, tr, frozenset({**vars_, idx: v}.items())))
                                else:
                                    vars_[idx] = v          # setvar
                            elif sub <= 0x8B:
                                vars_.pop(idx, None)        # other maths: value now unknown
                        else:
                            stop = True
                    elif cmd == 0xFE:
                        o += 2
                    elif cmd in (0xFB, 0xFC):
                        pass
                    elif cmd in (0xFD, 0xFF):               # return / end of track
                        stop = not cond
                    else:
                        stop = True
                    if pre in (0xA3, 0xA4, 0xA5):
                        o += {0xA3: 2, 0xA4: 4, 0xA5: 1}[pre]
                    if stop:
                        break
                except (IndexError, struct.error):
                    break
        return notes

    def inst_waves(self, base, reference, key, out):
        """Resolve an RBNK instrument reference to wave indices (appends to out)."""
        d = self.d
        typ = d[reference + 1]
        o = base + self.u32(reference + 4)
        if typ == 1:
            w = self.s32(o)
            if w >= 0 and w not in out:
                out.append(w)
        elif typ == 2:                              # key range table
            n = d[o]
            keys = d[o + 1:o + 1 + n]
            rt = o + 1 + n
            rt = (rt + 3) & ~3
            pick = None
            if key is not None:
                for i, k in enumerate(keys):
                    if key <= k:
                        pick = i
                        break
            idxs = range(n) if pick is None else [pick]
            for i in idxs:
                self.inst_waves(base, rt + 8 * i, None, out)
        elif typ == 3:                              # key index table
            lo, hi = d[o], d[o + 1]
            idxs = range(hi - lo + 1)
            if key is not None and lo <= key <= hi:
                idxs = [key - lo]
            for i in idxs:
                self.inst_waves(base, o + 4 + 8 * i, None, out)

    def seq_sound_waves(self, snd):
        """Return (rwar_offset, [wave indices]) for a SEQ-type sound."""
        det = self.ref(snd + 0x18)
        label, bank_id = self.u32(det), self.u32(det + 4)
        sloc = self.file_loc(self.u32(snd + 4))
        if not sloc or bank_id >= len(self.banks):
            return None, []
        bloc = self.file_loc(self.u32(self.banks[bank_id] + 4))
        if not bloc or not bloc[3]:
            return None, []
        mo = sloc[0]
        sd = mo + self.u32(mo + 0x10)
        seq_start = sd + self.u32(sd + 8)
        notes = self.seq_notes(seq_start, label)
        bo = bloc[0]
        bbase = bo + self.u32(bo + 0x10) + 8
        ninst = self.u32(bbase)
        waves = []
        self.last_seq_guessed = False
        for prg, key in sorted(notes, key=lambda n: tuple(-1 if x is None else x for x in n)):
            if prg is None:                         # instrument picked by the game at runtime
                self.last_seq_guessed = True
                progs = range(ninst)
            else:
                progs = [prg]
            for p in progs:
                if p < ninst:
                    self.inst_waves(bbase, bbase + 4 + 8 * p, key, waves)
        return bloc[2], waves


# --------------------------------------------------------------------------
# BRSTM (music)
# --------------------------------------------------------------------------

def decode_brstm(path):
    d = open(path, 'rb').read()
    if d[:4] != b'RSTM':
        return None
    u32 = lambda o: struct.unpack_from('>I', d, o)[0]
    head = u32(0x10)
    hb = head + 8
    si = hb + u32(hb + 4)
    ct = hb + u32(hb + 0x14)
    fmt, nch = d[si], d[si + 2]
    rate = struct.unpack_from('>H', d, si + 4)[0]
    total = u32(si + 0xC)
    data_off = u32(si + 0x10)
    nblocks, bsize, bsamp = u32(si + 0x14), u32(si + 0x18), u32(si + 0x1C)
    lbsize, lbpad = u32(si + 0x20), u32(si + 0x28)
    states = []
    for c in range(nch):
        cinfo = hb + u32(ct + 4 + 8 * c + 4)
        ai = hb + u32(cinfo + 4)
        coefs = struct.unpack_from('>16h', d, ai)
        h1, h2 = struct.unpack_from('>hh', d, ai + 0x24)
        states.append([coefs, h1, h2, array('h')])
    for b in range(nblocks):
        last = b == nblocks - 1
        size = lbpad if last else bsize
        nsamp = (total - bsamp * b) if last else bsamp
        for c in range(nch):
            off = data_off + b * bsize * nch + c * size if not last else data_off + b * bsize * nch + c * lbpad
            st = states[c]
            if fmt == 2:
                out, st[1], st[2] = decode_adpcm(d, off, nsamp, st[0], st[1], st[2])
            else:
                out = array('h', d[off:off + 2 * nsamp])
                if sys.byteorder == 'little':
                    out.byteswap()
            st[3].extend(out)
    return rate, [s[3] for s in states]


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-music', action='store_true', help='skip strm/*.brstm music')
    ap.add_argument('--only', help='only export folders whose path starts with this (e.g. Vehicles)')
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()

    t0 = time.time()
    b = Brsar(BRSAR)
    index = []
    ok = skipped = 0
    n = len(b.sounds)
    for i in range(n):
        name = b.sound_name(i)
        if 'DUMMY' in name or name.startswith('VOICE_LABEL'):
            continue
        folder = group_for(name)
        if args.only and not '/'.join(folder).lower().startswith(args.only.lower()):
            continue
        snd = b.sounds[i]
        typ = b.u8(snd + 0x16)
        if typ == 2:
            continue  # streams handled below from strm/
        try:
            if typ == 3:
                rwar, waves = b.wave_sound_waves(snd)
            else:
                rwar, waves = b.seq_sound_waves(snd)
        except Exception as e:  # keep going on odd entries
            print('  ! %d %s: %s' % (i, name, e))
            rwar, waves = None, []
        guessed = typ == 1 and b.last_seq_guessed
        note = ''
        if guessed and len(waves) > MAX_GUESSED:
            note = 'SKIPPED: game picks 1 of %d instruments at runtime' % len(waves)
            waves = []
        paths = []
        for k, w in enumerate(waves):
            res = b.rwar_wave(rwar, w)
            if not res:
                continue
            label = 'possible sample' if guessed else 'sample'
            fname = '%04d %s%s.wav' % (i, name, '' if len(waves) == 1 and not guessed
                                       else ' (%s %d)' % (label, k + 1))
            path = os.path.join(args.out, *[safe(x) for x in folder], safe(fname))
            if write_wav(path, res[0], res[1]):
                paths.append(os.path.relpath(path, args.out))
        if paths:
            ok += 1
        else:
            skipped += 1
        index.append([i, name, '/'.join(folder), {1: 'SEQ', 3: 'WAVE'}.get(typ, typ),
                      ' | '.join(paths) or note])
        if i % 250 == 0:
            print('%5d/%d  %s  (%.0fs)' % (i, n, name, time.time() - t0), flush=True)

    if not args.no_music and os.path.isdir(STRM_DIR):
        on_disk = {f.lower(): f for f in os.listdir(STRM_DIR)}
        streams = [i for i in range(n) if b.u8(b.sounds[i] + 0x16) == 2]
        cache = {}
        for j, i in enumerate(streams):
            name = b.sound_name(i)
            folder = group_for(name)
            if args.only and not '/'.join(folder).lower().startswith(args.only.lower()):
                continue
            ext = b.external_name(b.u32(b.sounds[i] + 4))
            f = on_disk.get(ext.split('/')[-1].lower())
            print('music %d/%d  %s <- %s  (%.0fs)' % (j + 1, len(streams), name, ext, time.time() - t0),
                  flush=True)
            if f is None:
                index.append([i, name, '/'.join(folder), 'STRM', 'MISSING ' + ext])
                skipped += 1
                continue
            if f not in cache:
                cache = {f: decode_brstm(os.path.join(STRM_DIR, f))}
            res = cache[f]
            paths = []
            if res:
                rate, chans = res
                # >2 channels = extra stereo layers (e.g. alternate instrument tracks)
                pairs = [chans[k:k + 2] for k in range(0, len(chans), 2)]
                for k, pr in enumerate(pairs):
                    fname = '%04d %s%s.wav' % (i, name, '' if len(pairs) == 1 else ' (layer %d)' % (k + 1))
                    path = os.path.join(args.out, *[safe(x) for x in folder], safe(fname))
                    if write_wav(path, rate, pr):
                        paths.append(os.path.relpath(path, args.out))
            index.append([i, name, '/'.join(folder), 'STRM', ' | '.join(paths)])
            if paths:
                ok += 1
            else:
                skipped += 1

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, 'index.csv'), 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['sound_id', 'name', 'folder', 'type', 'files'])
        w.writerows(index)
    print('Done: %d sounds exported, %d had no audio, %.0fs. Output: %s' % (ok, skipped, time.time() - t0, args.out))


if __name__ == '__main__':
    main()
