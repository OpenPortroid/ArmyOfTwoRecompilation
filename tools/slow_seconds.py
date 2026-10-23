import re, sys
# slow_seconds.py <prefix> [fps below] [tail seconds]: one row per second of a work/iraq_fight run
# (<prefix>.decouple.log, .native_gpu.log, .series.txt): FPS, engine tick, GPU ms/frame (busy, draws, transfers,
# idle between the frame's submits) and the loads of the game, rendering and GPU command threads. Rows: the seconds below
# the FPS given (default all) among the last N (default all).
prefix = sys.argv[1]; below = float(sys.argv[2]) if len(sys.argv) > 2 else 1e9
last = int(sys.argv[3]) if len(sys.argv) > 3 else 0


def sec(stamp):
    h, m, s = stamp.split(':'); return int(h) * 3600 + int(m) * 60 + float(s)


rows = []
for line in open(prefix + '.decouple.log', encoding='utf-8', errors='replace'):
    m = re.match(r'\[([\d:.]+)\] fps=([\d.]+) \(frame max ([\d.]+) ms\).*engine tick avg ([\d.]+) max ([\d.]+) ms'
                 r' \| draw-only (\d+), avg ([\d.]+)', line)
    if m:
        rows.append(dict(t=sec(m[1]), stamp=m[1][:8], fps=float(m[2]), fmax=float(m[3]), tick=float(m[4]),
                         tmax=float(m[5]), do=float(m[7])))
gpu = []
for line in open(prefix + '.native_gpu.log', encoding='utf-8', errors='replace'):
    m = re.search(r'gpu ms/frame: draw ([\d.]+) transfer ([\d.]+) resolve ([\d.]+).* total ([\d.]+) \[([\d:.]+)\]', line)
    if m:
        idle = re.search(r' idle ([\d.]+)', line)
        gpu.append((sec(m[5]), float(m[4]), float(m[1]), float(m[2]), float(idle[1]) if idle else 0.0))
threads = []
try:
    for line in open(prefix + '.series.txt', encoding='utf-8', errors='replace'):
        m = re.match(r'\[([\d:.]+)\] (.*)', line)
        if m:
            loads = {}
            for part in m[2].split(' | '):
                name, _, pct = part.rpartition(' ')
                loads[name] = float(pct)
            threads.append((sec(m[1]), loads))
except FileNotFoundError:
    pass


def near(series, t):
    best = min(series, key=lambda x: abs(x[0] - t), default=None)
    return best if best and abs(best[0] - t) < 0.8 else None


def load(loads, key):
    return max((v for n, v in loads.items() if key in n), default=0)


sel = rows[-last:] if last else rows
print('time      fps   fmax  tick  tmax  do   | gpu  draw  tr   idle | game render gpucmd present')
for r in sel:
    if r['fps'] >= below: continue
    g = near(gpu, r['t']); th = near(threads, r['t'])
    gs = '%4.2f %4.2f %4.2f %4.2f' % g[1:] if g else '  -                   '
    ts = ''
    if th:
        l = th[1]
        ts = '%4.0f %6.0f %6.0f %6.0f' % (load(l, 'Main XThread'), load(l, '(F8000088)'), load(l, 'GPU commands'),
                                         load(l, 'GPU present'))
    print('%s %5.1f %5.1f %4.2f %5.2f %4.2f | %s | %s' % (r['stamp'], r['fps'], r['fmax'], r['tick'], r['tmax'],
                                                         r['do'], gs, ts))
