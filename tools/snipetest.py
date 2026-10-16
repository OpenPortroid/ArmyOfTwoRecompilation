"""Co-op snipe aim test: orders the co-op snipe (RB + D-pad left), then turns
with the right stick and with the mouse, sampling the player controllers'
look rotation (guest memory) every 100 ms.

python snipetest.py <pid> <game_dir> [--no-order]
"""
import os
import struct
import sys
import time

exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ue3probe.py')).read().split('\ndef main')[0])

pid, game_dir = int(sys.argv[1]), sys.argv[2]
m = Mem(pid)


def name(idx):
    e = m.u32(m.u32(0x831D1724) + idx * 4)
    b = m.read(e + 16, 64) if e else b''
    return b.split(b'\0')[0].decode(errors='replace')


ctrls = [o for o in objects(m) if o and m.u32(o + 52) and name(m.u32(m.u32(o + 52) + 44)) == 'AO2PlayerController']
holders = [(c, m.u32(c + 2024)) for c in ctrls if m.u32(c + 2024)]
print('controllers', ['%08X' % c for c, h in holders])


def rots():
    return ['%7.0f/%7.0f' % struct.unpack('>2f', m.read(h + 3616, 8))[::-1] for c, h in holders]


def script(s):
    with open(os.path.join(game_dir, 'input.script'), 'w') as f:
        f.write(s)


def sample(label, seconds):
    print(label)
    t0 = time.time()
    while time.time() - t0 < seconds:
        print('  %4.1f s  yaw/pitch %s' % (time.time() - t0, '  '.join(rots())))
        time.sleep(0.1)


if '--no-order' not in sys.argv:
    script('rb+ wait600 left wait400 rb-')
    time.sleep(4)
script('rx20000 wait800 rx0')
sample('stick right 0.8 s', 2.5)
script(' '.join(['mx40 wait16'] * 50))
sample('mouse right 0.8 s', 2.5)
