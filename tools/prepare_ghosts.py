"""Make ghost files (.rkg) available to the web player.

Usage:
    python tools/prepare_ghosts.py [folder with .rkg files ...]     (default: samples/)

Copies every .rkg into web/assets/ghosts/ and writes ghosts.json, a list the page shows in its
"Ghost" menu. The header fields are read the way Kinoko's GhostFile does, so what the menu shows
(track, character, vehicle, time) is what the ghost will actually replay. Ghost files are player
data, not Nintendo assets; the samples ship with Kinoko.
"""

import json
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'web', 'assets', 'ghosts')


def read_time(v24):
    """MKWii 7-7-10 bit time (minutes, seconds, milliseconds) -> milliseconds."""
    return (v24 >> 17) * 60000 + ((v24 >> 10) & 0x7F) * 1000 + (v24 & 0x3FF)


def parse_rkg(data):
    """Header of an .rkg file, mirroring GhostFile::read (source/game/system/GhostFile.cc)."""
    if data[:4] != b'RKGD':
        raise ValueError('not a ghost file (missing RKGD)')
    w1, w2, w3 = struct.unpack_from('>III', data, 4)
    laps = data[0x10]
    lap_times = [read_time(int.from_bytes(data[0x11 + 3 * i:0x14 + 3 * i], 'big')) for i in range(min(laps, 5))]
    return {
        'course': (w1 >> 2) & 0x3F,
        'timeMs': read_time(w1 >> 8),
        'vehicle': w2 >> 26,
        'character': (w2 >> 20) & 0x3F,
        'date': '%04d-%02d-%02d' % (2000 + ((w2 >> 13) & 0x7F), (w2 >> 9) & 0xF, (w2 >> 4) & 0x1F),
        'driftAuto': bool((w3 >> 17) & 1),
        'compressed': bool((data[0x0C] >> 3) & 1),
        'laps': laps,
        'lapTimesMs': lap_times,
    }


def main():
    folders = sys.argv[1:] or [os.path.join(HERE, '..', 'samples')]
    os.makedirs(OUT, exist_ok=True)
    entries = []
    for folder in folders:
        for name in sorted(os.listdir(folder)):
            if not name.lower().endswith('.rkg'):
                continue
            data = open(os.path.join(folder, name), 'rb').read()
            try:
                info = parse_rkg(data)
            except ValueError as e:
                print('  ! %s: %s' % (name, e))
                continue
            shutil.copyfile(os.path.join(folder, name), os.path.join(OUT, name))
            info.update({'file': name, 'name': os.path.splitext(name)[0], 'size': len(data)})
            entries.append(info)
    with open(os.path.join(OUT, 'ghosts.json'), 'w') as f:
        json.dump(entries, f, indent=1)
    print('%d ghosts -> %s' % (len(entries), os.path.abspath(OUT)))
    for e in entries:
        t = e['timeMs']
        print('  %-28s course %2d char %2d veh %2d  %d\'%02d"%03d  laps %s' % (
            e['name'], e['course'], e['character'], e['vehicle'], t // 60000, t // 1000 % 60, t % 1000,
            [round(x / 1000, 3) for x in e['lapTimesMs']]))


if __name__ == '__main__':
    main()
