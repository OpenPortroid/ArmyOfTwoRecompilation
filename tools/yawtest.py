"""Mouse -> camera turn test: injects mouse movement at several speeds through
input.script and measures the player controller's yaw change (guest memory).

python yawtest.py <pid> [game_dir]
"""
import os
import struct
import sys
import time

exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ue3probe.py')).read().split('\ndef main')[0])

pid = int(sys.argv[1])
game_dir = sys.argv[2] if len(sys.argv) > 2 else r'C:\aot2\aot\out\build\win-amd64-release'
m = Mem(pid)


def name(idx):
    e = m.u32(m.u32(0x831D1724) + idx * 4)
    b = m.read(e + 16, 64) if e else b''
    return b.split(b'\0')[0].decode(errors='replace')


ctrl = None
for o in objects(m):
    if o and m.u32(o + 52) and name(m.u32(m.u32(o + 52) + 44)) == 'AO2PlayerController':
        ctrl = o
holder = m.u32(ctrl + 2024)
print('controller %08X holder %08X' % (ctrl, holder))


def yaw():
    return struct.unpack('>f', m.read(holder + 3616 + 4, 4))[0]


for per_step, steps in [(2, 60), (5, 60), (10, 60), (20, 60), (40, 30), (80, 15)]:
    script = ' '.join(['mx%d wait16' % per_step] * steps)
    y0 = yaw()
    with open(os.path.join(game_dir, 'input.script'), 'w') as f:
        f.write(script)
    time.sleep(steps * 0.016 + 2.5)
    y1 = yaw()
    total = per_step * steps
    d = y1 - y0
    print('%3d counts/step (%5.0f counts/s): %5d counts -> yaw %8.1f units = %6.3f units/count' %
          (per_step, per_step / 0.016, total, d, d / total))
    time.sleep(0.5)
