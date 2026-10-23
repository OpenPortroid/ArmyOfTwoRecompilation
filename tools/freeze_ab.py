import re, sys
# freeze_ab.py <prefix>: a FREEZE_AB run of work/iraq_fight.sh (<prefix>.ab.txt, .native_gpu.log, .decouple.log):
# FPS and GPU ms/frame by category of each state, averaged over its seconds after the first 2.


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
    m = re.match(r'\[([\d:.]+)\] fps=([\d.]+)', line)
    if m:
        fps.append((sec(m[1]), float(m[2])))

states = {}
for (t0, name), (t1, _) in zip(marks, marks[1:]):
    g = [c for t, c in gpu if t0 + 2.5 <= t <= t1 + 0.5]
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
