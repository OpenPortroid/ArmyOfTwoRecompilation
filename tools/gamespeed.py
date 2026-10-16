"""Game speed check: game time (WorldInfo.TimeSeconds) against real time,
plus the frame sync object's frame duration (+212, ms) and frame counter.

python gamespeed.py <pid> [seconds]
"""
import os
import struct
import sys
import time

exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ue3probe.py')).read().split('\ndef main')[0])

pid = int(sys.argv[1])
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
m = Mem(pid)


def name(idx):
    e = m.u32(m.u32(0x831D1724) + idx * 4)
    b = m.read(e + 16, 64) if e else b''
    return b.split(b'\0')[0].decode(errors='replace')


# Property offset of WorldInfo.TimeSeconds (UProperty offset at +0x64).
offset = None
worlds = []
for o in objects(m):
    if not o:
        continue
    n = name(m.u32(o + 44))
    if n == 'TimeSeconds' and name(m.u32(m.u32(o + 40) + 44)) == 'WorldInfo':
        offset = m.u32(o + 0x64)
    cls = m.u32(o + 52)
    if cls and name(m.u32(cls + 44)) == 'WorldInfo' and not name(n and m.u32(o + 44)).startswith('Default__'):
        worlds.append(o)
worlds = [w for w in worlds if not name(m.u32(w + 44)).startswith('Default__')]
sync = m.u32(0x831F3008)


def sample():
    t = [struct.unpack('>f', m.read(w + offset, 4))[0] for w in worlds]
    return t, m.u32(sync + 92), struct.unpack('>i', m.read(sync + 212, 4))[0]


print('TimeSeconds offset', offset, 'worlds', ['%08X' % w for w in worlds])
t0, f0, d0 = sample()
r0 = time.time()
time.sleep(seconds)
t1, f1, d1 = sample()
r1 = time.time()
real = r1 - r0
for a, b in zip(t0, t1):
    print('game time %.2f s in %.2f s real: speed x%.2f' % (b - a, real, (b - a) / real))
print('frames %d in %.2f s = %.1f/s, frame duration +212 = %d ms -> %d ms' % (f1 - f0, real, (f1 - f0) / real, d0, d1))
