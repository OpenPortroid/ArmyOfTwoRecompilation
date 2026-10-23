import re, sys
# freeze_ab.py <prefix>: a FREEZE_AB run of work/iraq_fight.sh (<prefix>.ab.txt, .native_gpu.log, .decouple.log):
# FPS and GPU ms/frame by category of each state, averaged over its seconds after the first 2.
# --live: a FREEZE_LIVE=1 run, the game was not frozen (every second counts).


def sec(stamp):
    h, m, s = stamp.split(':'); return int(h) * 3600 + int(m) * 60 + float(s)


prefix = sys.argv[1]
marks = []
for line in open(prefix + '.ab.txt'):
    parts = line.split()
    marks.append((sec(parts[0]), ' '.join(parts[1:])))
gpu = []
for line in open(prefix + '.native_gpu.log', encoding='utf-8', errors='replace'):
    m = re.search(r'gpu ms/frame:(.*)\| total ([\d.]+) \[([\d:.]+)\]', line)
    if m:
        cats = {k: float(v) for k, v in re.findall(r'(\w+) ([\d.]+)', m[1])}
        cats['total'] = float(m[2])
        gpu.append((sec(m[3]), cats))
fps = []
for line in open(prefix + '.decouple.log', encoding='utf-8', errors='replace'):
    m = re.match(r'\[([\d:.]+)\] fps=([\d.]+).*engine tick avg ([\d.]+)', line)
    if m:
        fps.append((sec(m[1]), float(m[2]), float(m[3])))
# seconds the game was not frozen (engine tick above 1.5 ms) don't count, unless --live (FREEZE_LIVE=1)
moving = [] if '--live' in sys.argv else [t for t, _, tick in fps if tick > 1.5]
if '--live' in sys.argv:
    fps = [(t, v, 0) for t, v, _ in fps]
fps = [(t, v) for t, v, tick in fps if tick <= 1.5]

states = {}
for (t0, name), (t1, _) in zip(marks, marks[1:]):
    if name == 'repause' or name == 'capture':
        continue
    g = [c for t, c in gpu if t0 + 2.5 <= t <= t1 + 0.5 and not any(abs(t - m) < 1.0 for m in moving)]
    f = [v for t, v in fps if t0 + 2.5 <= t <= t1 + 0.5]
    s = states.setdefault(name, {'gpu': [], 'fps': []})
    s['gpu'] += g; s['fps'] += f
    if g:
        print('%s %-40s fps %6.1f  gpu %.3f' % (
            '%02d:%02d:%02d' % (t0 // 3600, t0 // 60 % 60, t0 % 60), name, sum(f) / max(len(f), 1),
            sum(c['total'] for c in g) / len(g)))
print()
for name, s in states.items():
    g = s['gpu']
    if not g:
        continue
    keys = [k for k in g[0] if k != 'total']
    avg = {k: sum(c.get(k, 0) for c in g) / len(g) for k in keys + ['total']}
    print('%-40s fps %6.1f (%d s) | gpu %.3f | %s' % (name, sum(s['fps']) / max(len(s['fps']), 1), len(s['fps']),
          avg['total'], ' '.join('%s %.3f' % (k, avg[k]) for k in keys)))
