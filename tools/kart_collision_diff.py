"""Frame-by-frame check of Kinoko's kart-kart collision against a Dolphin recording.

The recording is a dump of DolphinLuaServer's scripts/kart_collision_logger.lua (out.exe -s ... -d dump.json),
taken while a two-player split-screen VS race plays back from a DTM movie. Two steps:

  1. Inputs for Kinoko, read from each racer's KPad in the dump:
       python tools/kart_collision_diff.py inputs dump.json versus_inputs.txt [--state-offset 8]
     then run the same race in Kinoko:
       kinoko_host versus --course <id> --kart <char>:<vehicle> --kart <char>:<vehicle> \
           --inputs versus_inputs.txt --out kinoko.csv --frames <n>
  2. Diff:
       python tools/kart_collision_diff.py diff dump.json kinoko.csv [--frame-shift 0]
  3. Item objects (vs_verify_logger dumps, kinoko_host versus --items):
       python tools/kart_collision_diff.py items dump.json kinoko_items.csv [--max-frame N]
  4. Course objects (vs_verify_logger dumps with course_object_logger.lua, kinoko_host versus --objects):
       python tools/kart_collision_diff.py objects dump.json kinoko_objects.csv [--max-frame N]
     Both sides log an object only when it changed. Game and Kinoko objects are paired by object id and starting
     position (Kinoko leaves some scenery out, so the list indices differ); position and transform are compared from race frame 1 on.

Frames are matched on the race frame counter (RaceManager +0x20 in the game, RaceManager::timer() in Kinoko). Floats
are compared exactly: the logger reads the game's single floats and Kinoko prints them with 9 significant digits.

KPad layout (checked on the first real dump, PAL): the current RaceInputState starts 0x28 bytes into the KPad
(--state-offset), laid out as Kinoko's RaceInputState: vtable, buttons u16, buttonsRaw u16, stick x f32, stick y f32,
stickXRaw u8, stickYRaw u8, trick u8, trickRaw u8.

Only racer slots below --players (default 2) are read: the logger's slot scan also returns junk slots past the last
racer.
"""
import argparse
import csv
import json
import struct

FIELDS = [  # (name in the Kinoko CSV, function of a dump racer)
    ('posX', lambda r: vec(r, 'pos', 0)), ('posY', lambda r: vec(r, 'pos', 1)), ('posZ', lambda r: vec(r, 'pos', 2)),
    ('rotX', lambda r: vec(r, 'full_rot', 0)), ('rotY', lambda r: vec(r, 'full_rot', 1)),
    ('rotZ', lambda r: vec(r, 'full_rot', 2)), ('rotW', lambda r: vec(r, 'full_rot', 3)),
    ('speed', lambda r: r.get('speed')),
    ('playerBumpTimer', lambda r: s16(r.get('bump_timer'))),
    ('extVelX', lambda r: vec(r, 'ext_vel', 0)), ('extVelY', lambda r: vec(r, 'ext_vel', 1)),
    ('extVelZ', lambda r: vec(r, 'ext_vel', 2)), ('angVel0Z', lambda r: vec(r, 'ang_vel0', 2)),
    ('slipstream', lambda r: slipstream(r)),
    # Held item (KartItem +0x88 inventory: id, 0x14 = none, and count), logged by vs_verify_logger.lua.
    ('item', lambda r: (r.get('_held') or {}).get('id')),
    ('itemCount', lambda r: (r.get('_held') or {}).get('count')),
]


def slipstream(racer):
    """0 / 1 charging / 2 boosting, from status word 2 (KartState +0xC) bits 0 and 1."""
    words = racer.get('status')
    if not words:
        return None
    w = int(words['3'])
    return 2 if w & 2 else (1 if w & 1 else 0)


def f32(x):
    """Rounds to single precision: the game's floats arrive as doubles, Kinoko's as 9-digit decimals."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def vec(racer, key, i):
    v = racer.get(key)
    return None if v is None else v.get(str(i + 1))


def s16(v):
    if v is None:
        return None
    v = int(v)
    return v - 0x10000 if v >= 0x8000 else v


def load_dump(path, players=2):
    """{race frame: {slot: racer}} from a logger dump (a list of {"data": ...}, arrays as {"1": ..})."""
    frames = {}
    for entry in json.load(open(path)):
        data = entry.get('data', entry)
        if 'frame_count' not in data or not data.get('racers'):
            continue
        racers = {int(r['slot']): r for r in data['racers'].values() if int(r['slot']) < players}
        for slot, racer in racers.items():
            racer['_held'] = (data.get('held') or {}).get(str(slot + 1))
        frames[int(data['frame_count'])] = racers
    return frames


def pad_state(racer, offset):
    words = racer.get('pad')
    if not words:
        return None
    raw = b''.join(struct.pack('>I', int(words[str(i + 1)]) & 0xFFFFFFFF) for i in range(len(words)))
    buttons, = struct.unpack_from('>H', raw, offset + 4)
    stick_x, stick_y = struct.unpack_from('>ff', raw, offset + 8)
    trick = raw[offset + 0x12]
    return buttons, stick_x, stick_y, trick


def cmd_inputs(args):
    frames = load_dump(args.dump, args.players)
    with open(args.out, 'w') as out:
        last = {}
        for frame in sorted(frames):
            for slot, racer in sorted(frames[frame].items()):
                state = pad_state(racer, args.state_offset)
                if state is None or last.get(slot) == state:
                    continue
                last[slot] = state
                # The logged state is the one the game used for this race frame; Kinoko applies input lines before
                # simulating the frame with the same index.
                out.write('%d %d %d %.9g %.9g %d\n' % (frame + args.frame_shift, slot, *state))
    print('wrote %s for %d frames' % (args.out, len(frames)))


def cmd_diff(args):
    frames = load_dump(args.dump, args.players)
    kinoko = {}
    for row in csv.DictReader(open(args.kinoko)):
        kinoko[(int(row['raceFrame']) + args.frame_shift, int(row['player']))] = row

    first = {}
    compared = 0
    for frame in sorted(frames):
        for slot, racer in frames[frame].items():
            row = kinoko.get((frame, slot))
            if row is None:
                continue
            compared += 1
            for name, get in FIELDS:
                want = get(racer)
                if want is None or (slot, name) in first:
                    continue
                got = f32(float(row[name]))
                want = f32(float(want))
                if want != got:
                    first[(slot, name)] = (frame, float(want), got)

    print('compared %d kart-frames' % compared)
    if not first:
        print('no differences')
        return
    for (slot, name), (frame, want, got) in sorted(first.items(), key=lambda kv: kv[1][0]):
        print('kart %d %-16s first differs at race frame %d: dolphin %.9g kinoko %.9g' % (slot, name, frame, want, got))


def cmd_items(args):
    """Item objects by (kind, index) per race frame: positions as single floats, bit-exact or not."""
    kinoko = {}
    for row in csv.DictReader(open(args.kinoko)):
        kinoko.setdefault(int(row['raceFrame']), []).append(row)

    frames = {}
    for entry in json.load(open(args.dump)):
        data = entry.get('data', entry)
        if 'frame_count' not in data or not data.get('racers'):
            continue
        frame = int(data['frame_count'])
        if frame and frame not in frames:
            frames[frame] = data.get('item_objs') or {}

    compared = exact = 0
    first = {}
    for frame in sorted(frames):
        if args.max_frame and frame > args.max_frame:
            break
        for obj in frames[frame].values():
            key = (int(obj['kind']), int(obj['idx']))
            match = [r for r in kinoko.get(frame, []) if (int(r['kind']), int(r['idx'])) == key]
            if not match:
                first.setdefault(('missing', key), frame)
                continue
            compared += 1
            game = [f32(obj['pos'][str(i + 1)]) for i in range(3)]
            mine = [f32(float(match[0][c])) for c in ('posX', 'posY', 'posZ')]
            if game == mine:
                exact += 1
            else:
                first.setdefault(('pos', key), (frame, game, mine))
    print('compared %d item-object frames, %d bit-exact' % (compared, exact))
    for key, info in sorted(first.items(), key=lambda kv: kv[1] if isinstance(kv[1], int) else kv[1][0]):
        print('  first %s kind %d idx %d: %s' % (key[0], key[1][0], key[1][1], info))


OBJECT_COLUMNS = ['posX', 'posY', 'posZ'] + ['m%d%d' % (r, c) for r in range(3) for c in range(4)]


def _object_timelines_game(path):
    """{list index: [(race frame, id, values)]} from the dump's per-frame 'objects' deltas."""
    lines = {}
    frames = set()
    for entry in json.load(open(path)):
        data = entry.get('data', entry)
        if 'frame_count' not in data or not data.get('racers'):
            continue
        frame = int(data['frame_count'])
        frames.add(frame)
        objs = data.get('objects') or {}
        for e in (objs.get('changed') or {}).values():
            pos, mat = e.get('pos'), e.get('mat')
            if pos is None or mat is None:
                continue
            values = [f32(pos[str(i + 1)]) for i in range(3)] + [f32(mat[str(i + 1)]) for i in range(12)]
            lines.setdefault(int(e['i']), []).append((frame, int(e.get('id', -1)), values))
    return lines, frames


def _object_timelines_kinoko(path):
    lines = {}
    for row in csv.DictReader(open(path)):
        values = [f32(float(row[c])) for c in OBJECT_COLUMNS]
        line = lines.setdefault(int(row['idx']), [])
        frame = int(row['raceFrame'])
        if line and line[-1][0] == frame:
            line[-1] = (frame, int(row['id']), values)
        else:
            line.append((frame, int(row['id']), values))
    return lines


def _value_at(line, frame, cursor):
    """The last entry of `line` at or before `frame`; `cursor` keeps the position between calls."""
    while cursor[0] + 1 < len(line) and line[cursor[0] + 1][0] <= frame:
        cursor[0] += 1
    return line[cursor[0]] if line[cursor[0]][0] <= frame else None


def cmd_objects(args):
    """Course objects: position and transform per race frame, game against Kinoko."""
    game, frames = _object_timelines_game(args.dump)
    mine = _object_timelines_kinoko(args.kinoko)
    unused = dict(mine)
    pairs = []
    for gi in sorted(game):
        # Pair with the unused Kinoko object of the same id nearest to it on race frame 1 (race frame 0 spans the
        # intro, several VIs in the game, so objects that move during the intro are not comparable there).
        gid = game[gi][0][1]
        gframe = max(1, game[gi][0][0])
        gvalues = (_value_at(game[gi], gframe, [0]) or game[gi][0])[2]
        best = None
        for ki, line in unused.items():
            k = _value_at(line, gframe, [0]) or line[0]
            if k[1] != gid:
                continue
            dist = sum((a - b) ** 2 for a, b in zip(k[2][:3], gvalues[:3]))
            if best is None or dist < best[0]:
                best = (dist, ki)
        if best is not None and best[0] < 100.0 ** 2:
            pairs.append((gi, best[1]))
            del unused[best[1]]
    paired_ids = {game[gi][0][1] for gi, _ in pairs}
    unmatched = sorted({game[gi][0][1] for gi in game} - paired_ids)
    print('game objects %d, Kinoko objects %d, paired %d' % (len(game), len(mine), len(pairs)))
    if unmatched:
        print('game object ids with no Kinoko partner: %s' % ' '.join('0x%x' % i for i in unmatched))
    compared = exact = 0
    first = {}
    for gi, ki in pairs:
        gc, kc = [0], [0]
        for frame in sorted(frames):
            if frame < 1:
                continue
            if args.max_frame and frame > args.max_frame:
                break
            g = _value_at(game[gi], frame, gc)
            k = _value_at(mine[ki], frame, kc)
            if g is None or k is None:
                continue
            compared += 1
            if g[2] == k[2]:
                exact += 1
            elif (gi, ki) not in first:
                col = next(c for c in range(len(OBJECT_COLUMNS)) if g[2][c] != k[2][c])
                first[(gi, ki)] = (frame, g[1], OBJECT_COLUMNS[col], g[2][col], k[2][col])
    print('compared %d object-frames, %d bit-exact' % (compared, exact))
    for (gi, ki), (frame, oid, col, want, got) in sorted(first.items(), key=lambda kv: kv[1][0]):
        print('  object id 0x%x (game %d, Kinoko %d) first differs at race frame %d: %s dolphin %.9g kinoko %.9g'
              % (oid, gi, ki, frame, col, want, got))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('inputs')
    a.add_argument('dump')
    a.add_argument('out')
    a.add_argument('--state-offset', type=lambda s: int(s, 0), default=0x28)
    a.add_argument('--players', type=int, default=2)
    a.add_argument('--frame-shift', type=int, default=0)
    d = sub.add_parser('diff')
    d.add_argument('dump')
    d.add_argument('kinoko')
    d.add_argument('--frame-shift', type=int, default=0)
    d.add_argument('--players', type=int, default=2)
    i = sub.add_parser('items')
    i.add_argument('dump')
    i.add_argument('kinoko')
    i.add_argument('--max-frame', type=int, default=0)
    o = sub.add_parser('objects')
    o.add_argument('dump')
    o.add_argument('kinoko')
    o.add_argument('--max-frame', type=int, default=0)
    args = p.parse_args()
    {'inputs': cmd_inputs, 'diff': cmd_diff, 'items': cmd_items, 'objects': cmd_objects}[args.cmd](args)


if __name__ == '__main__':
    main()
