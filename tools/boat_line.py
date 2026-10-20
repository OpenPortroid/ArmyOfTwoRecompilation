"""A racing line down the river for the boat autopilot: shorter than the GPS line, inside the water with a margin from
land and buoys, smooth enough for the boat at speed.

    python tools/boat_line.py track.json [--margin 300] [--out work/boat_line.path] [--png ...]

1. The shortest path over the water on the track's grid (8 neighbours, cells at least `margin` from land and buoys;
   scipy dijkstra) from where the simulation starts (after the cutscene) to the end of the line (the dock).
2. An elastic band: resampled every --step units, each point pulled towards the middle of its neighbours (shorter,
   smoother) and pushed away from land / buoys where it comes within `margin` (the clearance gradient), many rounds.
Writes an autopilot.path (x y z per line) and <out>.track.json (the track with this line, for boat_sim.py).
"""
import argparse
import json
import math

import numpy as np
from scipy.ndimage import map_coordinates
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import dijkstra

import boat_sim


def clearance_map(track):
    g = track['grid']
    clear = np.array(track['clearance'], dtype=np.float32)
    jj, ii = np.mgrid[0:g['ny'], 0:g['nx']]
    for ox, oy, r in track['obstacles']:
        clear = np.minimum(clear, np.hypot(g['x0'] + ii * g['cell'] - ox, g['y0'] + jj * g['cell'] - oy) - r)
    return clear


def shortest(track, clear, margin, start, end):
    g = track['grid']
    ok = clear >= margin
    ny, nx = ok.shape
    idx = -np.ones((ny, nx), int)
    idx[ok] = np.arange(ok.sum())
    rows, cols, w = [], [], []
    for dj, di in ((0, 1), (1, 0), (1, 1), (1, -1)):
        j0, j1 = 0, ny - dj
        i0, i1 = max(0, -di), nx - max(0, di)
        a, ia = ok[j0:j1, i0:i1], idx[j0:j1, i0:i1]
        b, ib = ok[j0 + dj:j1 + dj, i0 + di:i1 + di], idx[j0 + dj:j1 + dj, i0 + di:i1 + di]
        m = a & b
        rows.append(ia[m])
        cols.append(ib[m])
        w.append(np.full(int(m.sum()), math.hypot(di, dj) * g['cell']))
    graph = coo_matrix((np.concatenate(w), (np.concatenate(rows), np.concatenate(cols))),
                       shape=(ok.sum(), ok.sum())).tocsr()
    cells = np.argwhere(ok)  # (j, i) per node index

    def node(x, y):
        j, i = (y - g['y0']) / g['cell'], (x - g['x0']) / g['cell']
        return int(np.argmin((cells[:, 0] - j) ** 2 + (cells[:, 1] - i) ** 2))

    s, e = node(*start), node(*end)
    dist, pred = dijkstra(graph, directed=False, indices=s, return_predecessors=True)
    path, k = [], e
    while k >= 0 and k != s:
        path.append(k)
        k = pred[k]
    path.append(s)
    path.reverse()
    pts = [(g['x0'] + cells[k, 1] * g['cell'], g['y0'] + cells[k, 0] * g['cell']) for k in path]
    return pts, dist[e]


def band(pts, clear, g, margin, step, rounds=3000, pull=0.3, stiff=0.0):
    line = boat_sim.Line(pts)
    n = int(line.length // step) + 1
    p = np.array([line.at(i * line.length / (n - 1)) for i in range(n)])
    gy, gx = np.gradient(clear.astype(np.float64), g['cell'])

    def sample(arr, q):
        return map_coordinates(arr, [(q[:, 1] - g['y0']) / g['cell'], (q[:, 0] - g['x0']) / g['cell']], order=1,
                               mode='nearest')

    for it in range(rounds):
        mid = 0.5 * (p[:-2] + p[2:])
        p[1:-1] += pull * (mid - p[1:-1])
        if stiff > 0:
            # bending stiffness: the 4th difference (what makes bends wider, not the line shorter)
            d4 = p[:-4] - 4 * p[1:-3] + 6 * p[2:-2] - 4 * p[3:-1] + p[4:]
            p[2:-2] -= stiff * d4
        c = sample(clear, p)
        close = c < margin
        if close.any():
            grad = np.stack([sample(gx, p), sample(gy, p)], axis=1)
            norm = np.linalg.norm(grad, axis=1)[:, None] + 1e-6
            push = np.where(close, margin - c, 0.0)[:, None] * grad / norm
            push[0] = push[-1] = 0
            p += push * 0.8
        if it % 500 == 0 or it == rounds - 1:
            seg = np.linalg.norm(np.diff(p, axis=0), axis=1)
            # keep the spacing even: resample now and then (the pull bunches points on bends)
            line = boat_sim.Line([tuple(q) for q in p])
            p = np.array([line.at(i * line.length / (n - 1)) for i in range(n)])
    return p


def stats(p, clear, g):
    d = np.diff(p, axis=0)
    seg = np.linalg.norm(d, axis=1)
    h = np.arctan2(d[:, 1], d[:, 0])
    turn = np.abs(np.remainder(np.diff(h) + np.pi, 2 * np.pi) - np.pi)
    radius = 0.5 * (seg[1:] + seg[:-1]) / np.maximum(turn, 1e-9)
    c = map_coordinates(clear, [(p[:, 1] - g['y0']) / g['cell'], (p[:, 0] - g['x0']) / g['cell']], order=1)
    return 'length %.0f, tightest bend radius %.0f (5%%: %.0f), closest to land/buoy %.0f' % (
        seg.sum(), radius.min(), np.percentile(radius, 5), c.min())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('track')
    ap.add_argument('--margin', type=float, default=300.0)
    ap.add_argument('--step', type=float, default=150.0)
    ap.add_argument('--rounds', type=int, default=3000)
    ap.add_argument('--pull', type=float, default=0.3, help='shortening (towards the middle of the neighbours)')
    ap.add_argument('--stiff', type=float, default=0.0, help='bending stiffness (0..0.15)')
    ap.add_argument('--z', type=float, default=-769.0)
    ap.add_argument('--out', default='work/boat_line.path')
    ap.add_argument('--png')
    a = ap.parse_args()
    with open(a.track) as f:
        track = json.load(f)
    g = track['grid']
    clear = clearance_map(track)
    line = boat_sim.Line(track['line'])
    s0 = track.get('start_s', 0.0)
    start, end = line.at(s0), line.at(line.length)
    gps = np.array([line.at(s) for s in np.arange(s0, line.length, a.step)] + [end])
    print('GPS line: ' + stats(gps, clear, g))
    pts, d = shortest(track, clear, a.margin, start, end)
    pts = [start] + pts[1:-1] + [end]
    print('grid shortest path: %.0f' % d)
    p = band(pts, clear, g, a.margin, a.step, a.rounds, a.pull, a.stiff)
    print('racing line: ' + stats(p, clear, g))
    with open(a.out, 'w') as f:
        f.write('# racing line, margin %.0f\n' % a.margin)
        for x, y in p:
            f.write('%.1f %.1f %.1f\n' % (x, y, a.z))
    track2 = dict(track, line=[tuple(map(float, q)) for q in p], start_s=0.0)
    with open(a.out + '.track.json', 'w') as f:
        json.dump(track2, f)
    print('%d points -> %s' % (len(p), a.out))
    if a.png:
        boat_sim.plot(track, a.png, [], [('racing line', [(0, x, y) for x, y in p], dict(time=0, hit=False))],
                      None, 'racing line (dashed) vs the GPS line (yellow), margin %.0f' % a.margin)


if __name__ == '__main__':
    main()
