"""The autopilot's driving (autopilot.cpp Drive) run on the fitted boat model (boat_model.py) along the river, to tune
its settings off the game.

    python tools/boat_sim.py track run.csv --level work/level [--out work/boat_track.json]
        the route line (the projections the telemetry logged, its pieces stitched) and the water: the land
        (tools/level_dump.py terrain: no hole, above the water) rasterized with the distance to it, the floating
        obstacles (buoys) as circles
    python tools/boat_sim.py run model.json track.json [--set name=value ...]   one simulated drive
    python tools/boat_sim.py search model.json track.json [--iters 400]          random search for the fastest settings

A simulated drive starts where the telemetry resumed after the river's cutscene (it moves the boat) and ends at the
end of the line (time) or when the boat comes within BOAT_RADIUS of land or an obstacle (a hit: the time so far plus a
penalty and the distance still to go at 600 units/s).
"""
import argparse
import bisect
import csv
import json
import math
import os
import random

import boat_model

DEFAULTS = dict(points=4, lookahead=600.0, lookahead_time=1.2, steer=2.0, gas=1.0, min_gas=0.33, corner=0.0,
                corner_time=2.0, slip=0.0, predict=0.0)
RANGES = dict(points=(1, 10), lookahead=(200.0, 1500.0), lookahead_time=(0.3, 2.5), steer=(0.5, 6.0),
              min_gas=(0.0, 0.8), corner=(0.0, 3.0), corner_time=(0.5, 4.0), slip=(0.0, 1.0), predict=(0.0, 0.8))
HIT_PENALTY = 3.0
# the game bumps the boat on banks 300-500 units from the terrain water edge: BOAT_RADIUS=350 is the honest check
BOAT_RADIUS = float(os.environ.get("BOAT_RADIUS", 200.0))
WATER_LEVEL = -720.0
CELL = 64.0
DT = 1.0 / 30


# ---- the track

def stitch_line(rows):
    """The route projections in order, every 50 units; a jump (the next piece of the route) cuts the line back to the
    point nearest the new piece. Also where the drive resumed after the longest pause (the cutscene)."""
    pts, resume, gap = [], None, 0.0
    prev_t = None
    for r in rows:
        if r['off'] < 0 or not (r['route_x'] or r['route_y']):
            continue
        p = (r['route_x'], r['route_y'])
        if prev_t is not None and r['t'] - prev_t > gap and pts:
            gap, resume = r['t'] - prev_t, p
        prev_t = r['t']
        if not pts:
            pts.append(p)
            continue
        d = math.hypot(p[0] - pts[-1][0], p[1] - pts[-1][1])
        if d > 500:
            k = min(range(len(pts)), key=lambda i: math.hypot(pts[i][0] - p[0], pts[i][1] - p[1]))
            if math.hypot(pts[k][0] - p[0], pts[k][1] - p[1]) < 1500:
                del pts[k + 1:]
            pts.append(p)
        elif d >= 50:
            pts.append(p)
    return pts, resume


def land_raster(level, line_pts, pad=6000.0):
    """Land (True) on a CELL grid over the line's box; where no terrain covers a cell it counts as water."""
    import numpy as np
    terr = np.load(level + '.npz')
    xs = [p[0] for p in line_pts]
    ys = [p[1] for p in line_pts]
    x0, y0 = min(xs) - pad, min(ys) - pad
    nx, ny = int((max(xs) + pad - x0) / CELL) + 1, int((max(ys) + pad - y0) / CELL) + 1
    gx = x0 + np.arange(nx) * CELL
    gy = y0 + np.arange(ny) * CELL
    land = np.zeros((ny, nx), bool)
    for k in sorted({f.rsplit('_', 1)[0] for f in terr.files}):
        z, holes, geo = terr[k + '_z'], terr[k + '_holes'], terr[k + '_geo']
        tny, tnx = z.shape
        fi = (gx - geo[0]) / geo[2]
        fj = (gy - geo[1]) / geo[3]
        ii = np.clip(np.round(fi).astype(int), 0, tnx - 1)
        jj = np.clip(np.round(fj).astype(int), 0, tny - 1)
        inside = ((fi >= 0) & (fi <= tnx - 1))[None, :] & ((fj >= 0) & (fj <= tny - 1))[:, None]
        zz = z[jj[:, None], ii[None, :]]
        hh = holes[jj[:, None], ii[None, :]]
        land |= inside & ~hh & (zz > WATER_LEVEL)
    return land, (x0, y0, nx, ny)


def build_track(path, level):
    import numpy as np
    from scipy.ndimage import distance_transform_edt
    pts, resume = stitch_line(boat_model.load(path))
    land, (x0, y0, nx, ny) = land_raster(level, pts)
    clearance = distance_transform_edt(~land) * CELL  # distance to the nearest land cell
    with open(level + '.json') as f:
        obstacles = [o for o in json.load(f) if 'buoy' in o['mesh'].lower() and o['radius'] > 0 and (o['x'] or o['y'])]
    line = Line(pts)
    start_s = line.locate(*resume)[0] if resume else 0.0
    return dict(line=pts, start_s=start_s, grid=dict(x0=x0, y0=y0, nx=nx, ny=ny, cell=CELL),
                clearance=np.round(clearance).astype(int).tolist(),
                obstacles=[(o['x'], o['y'], o['radius']) for o in obstacles])


class Water:
    def __init__(self, track):
        import numpy as np
        g = track['grid']
        self.x0, self.y0, self.cell = g['x0'], g['y0'], g['cell']
        self.clear = np.array(track['clearance'], dtype=np.float32)
        self.obstacles = track['obstacles']

    def hit(self, x, y):
        i, j = int((x - self.x0) / self.cell + 0.5), int((y - self.y0) / self.cell + 0.5)
        ny, nx = self.clear.shape
        if 0 <= i < nx and 0 <= j < ny and self.clear[j, i] < BOAT_RADIUS:
            return 'land'
        for ox, oy, r in self.obstacles:
            if (x - ox) ** 2 + (y - oy) ** 2 < (r + BOAT_RADIUS) ** 2:
                return 'buoy'
        return None


class Line:
    def __init__(self, pts):
        self.pts = pts
        self.cum = [0.0]
        for a, b in zip(pts, pts[1:]):
            self.cum.append(self.cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
        self.length = self.cum[-1]
        self.hint = 0

    def locate(self, x, y, near=None):
        """(distance along, signed offset + = right) of the nearest point; `near`: search around that distance."""
        lo, hi = 0, len(self.pts) - 1
        if near is not None:
            i = bisect.bisect_left(self.cum, near)
            lo, hi = max(0, i - 40), min(len(self.pts) - 1, i + 60)
        best = (float('inf'), 0.0, 0.0)
        for i in range(lo, hi):
            (ax, ay), (bx, by) = self.pts[i], self.pts[i + 1]
            dx, dy = bx - ax, by - ay
            l2 = dx * dx + dy * dy or 1.0
            t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / l2))
            px, py = ax + dx * t, ay + dy * t
            d2 = (x - px) ** 2 + (y - py) ** 2
            if d2 < best[0]:
                side = dx * (y - ay) - dy * (x - ax)  # + = right of the line (y to the right of x)
                best = (d2, self.cum[i] + t * math.sqrt(l2), math.copysign(math.sqrt(d2), side))
        return best[1], best[2]

    def at(self, s):
        s = max(0.0, min(self.length, s))
        i = max(1, bisect.bisect_left(self.cum, s))
        a, b = self.pts[i - 1], self.pts[i]
        seg = self.cum[i] - self.cum[i - 1] or 1.0
        t = (s - self.cum[i - 1]) / seg
        return a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t


# ---- the controller (as autopilot.cpp Drive)

def drive(model, track, cfg, start=None, max_time=400.0, log=None):
    p = [model[k] for k in boat_model.PARAMS]
    line = Line(track['line'])
    water = Water(track)
    s0 = track.get('start_s', 0.0)
    x, y = line.at(s0) if start is None else start
    bx, by = line.at(s0 + 60)
    st = dict(x=x, y=y, yaw=math.atan2(by - y, bx - x), u=0.0, v=0.0, r=0.0)
    s_now, t = s0, 0.0
    while t < max_time:
        yaw = st['yaw']
        c, s = math.cos(yaw), math.sin(yaw)
        vx, vy = st['u'] * c - st['v'] * s, st['u'] * s + st['v'] * c
        speed = math.hypot(vx, vy)
        s_now, off = line.locate(st['x'], st['y'], s_now)
        what = water.hit(st['x'], st['y'])
        if what:
            return dict(time=t + HIT_PENALTY + (line.length - s_now) / 600.0, hit=what, at=s_now, t=t)
        if line.length - s_now < 300:
            return dict(time=t, hit=False, at=s_now, t=t)
        px, py = st['x'] + vx * cfg['predict'], st['y'] + vy * cfg['predict']
        sp, _ = line.locate(px, py, s_now)
        ahead = max(cfg['lookahead'], speed * cfg['lookahead_time'])
        n = int(cfg['points'])
        tx = ty = 0.0
        for i in range(1, n + 1):
            ax_, ay_ = line.at(sp + ahead * i / n)
            tx += ax_ / n
            ty += ay_ / n
        bend = 0.0
        if cfg['corner'] > 0:
            far = max(400.0, speed * cfg['corner_time'])
            a0, a1, a2 = line.at(sp), line.at(sp + far / 2), line.at(sp + far)
            bend = abs(math.remainder(math.atan2(a2[1] - a1[1], a2[0] - a1[0]) -
                                      math.atan2(a1[1] - a0[1], a1[0] - a0[0]), 2 * math.pi))
        hy = yaw
        if cfg['slip'] > 0 and speed > 200:
            hy = math.atan2((1 - cfg['slip']) * math.sin(yaw) + cfg['slip'] * vy / speed,
                            (1 - cfg['slip']) * math.cos(yaw) + cfg['slip'] * vx / speed)
        dx, dy = tx - px, ty - py
        err = math.atan2(-dx * math.sin(hy) + dy * math.cos(hy), dx * math.cos(hy) + dy * math.sin(hy))
        steer = max(-1.0, min(1.0, err * cfg['steer']))
        gas = cfg['gas'] * max(cfg['min_gas'], min(1.0, 1.0 - (abs(err) - 0.5) / 1.5))
        if bend > 0.35:
            gas *= max(cfg['min_gas'], min(1.0, 1.0 - cfg['corner'] * (bend - 0.35)))
        if log is not None:
            log.append((t, st['x'], st['y'], speed, steer, gas, off))
        st = boat_model.step(p, st, steer, gas, 0, DT)
        t += DT
    return dict(time=max_time, hit=False, at=s_now, t=t)


def plot(track, out, real=(), sims=(), box=None, title=''):
    """The water (clearance to land, white = land), the route line, buoys, real drives (csv) and simulated ones
    (lists of (t, x, y, ...)), the hits marked. `box`: (x0, y0, x1, y1) to zoom."""
    import numpy as np
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    g = track['grid']
    clear = np.array(track['clearance'], dtype=np.float32)
    ext = (g['x0'], g['x0'] + g['nx'] * g['cell'], g['y0'], g['y0'] + g['ny'] * g['cell'])
    w, h = (ext[1] - ext[0], ext[3] - ext[2]) if not box else (box[2] - box[0], box[3] - box[1])
    fig, ax = plt.subplots(figsize=(10, max(4, min(30, 10 * h / w))))
    img = np.ma.masked_where(clear <= 0, np.minimum(clear, 1500))
    ax.imshow(img, origin='lower', extent=ext, cmap='Blues', vmin=-300, vmax=1500)
    ax.imshow(np.ma.masked_where(clear > BOAT_RADIUS, clear), origin='lower', extent=ext, cmap='Oranges',
              vmin=0, vmax=BOAT_RADIUS * 2, alpha=0.5)
    line = track['line']
    ax.plot([q[0] for q in line], [q[1] for q in line], color='gold', lw=1.2, label='GPS line')
    for ox, oy, r in track['obstacles']:
        ax.add_patch(plt.Circle((ox, oy), r, color='red', fill=True))
        ax.add_patch(plt.Circle((ox, oy), r + BOAT_RADIUS, color='red', fill=False, lw=0.6))
    colors = ['tab:green', 'tab:purple', 'tab:brown', 'tab:pink', 'tab:olive', 'tab:cyan']
    for i, path in enumerate(real):
        rows = boat_model.load(path)
        ax.plot([r['x'] for r in rows], [r['y'] for r in rows], color=colors[i % len(colors)], lw=0.8,
                label='game: ' + path.replace('\\', '/').split('/')[-1])
    for i, (name, log, res) in enumerate(sims):
        ax.plot([q[1] for q in log], [q[2] for q in log], color=['black', 'magenta', 'darkorange'][i % 3], lw=1.2,
                ls='--', label='sim %s: %.1f s%s' % (name, res['time'], ' (hit %s)' % res['hit'] if res['hit'] else ''))
        if res['hit'] and log:
            ax.plot(log[-1][1], log[-1][2], 'rx', ms=12, mew=2)
    if box:
        ax.set_xlim(box[0], box[2])
        ax.set_ylim(box[1], box[3])
    ax.set_aspect('equal')
    ax.set_title(title)
    ax.legend(loc='upper right', fontsize=7)
    fig.savefig(out, dpi=110, bbox_inches='tight')
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('track')
    p.add_argument('csv')
    p.add_argument('--level', default='work/level')
    p.add_argument('--out', default='work/boat_track.json')
    for name in ('run', 'search'):
        p = sub.add_parser(name)
        p.add_argument('model')
        p.add_argument('track')
        p.add_argument('--set', action='append', default=[])
        p.add_argument('--iters', type=int, default=400)
        p.add_argument('--seed', type=int, default=1)
        p.add_argument('--png')
        p.add_argument('--real', action='append', default=[])
        p.add_argument('--zoom', type=float, default=0, help='--png around the hit, this many units each way')
    a = ap.parse_args()
    if a.cmd == 'track':
        tr = build_track(a.csv, a.level)
        with open(a.out, 'w') as f:
            json.dump(tr, f)
        line = Line(tr['line'])
        water = Water(tr)
        on_land = sum(1 for k in range(0, int(line.length), 100) if water.hit(*line.at(k)))
        print('line %.0f units from %.0f, %d buoys, the line itself within %.0f of land at %d of %d points -> %s' % (
            line.length, tr['start_s'], len(tr['obstacles']), BOAT_RADIUS, on_land, int(line.length / 100), a.out))
        return
    with open(a.model) as f:
        model = json.load(f)
    with open(a.track) as f:
        track = json.load(f)
    cfg = dict(DEFAULTS)
    for kv in a.set:
        k, v = kv.split('=')
        cfg[k] = float(v)
    if a.cmd == 'run':
        print(cfg)
        log = []
        res = drive(model, track, cfg, log=log)
        print(res)
        if a.png:
            box = None
            if a.zoom and log:
                x, y = log[-1][1], log[-1][2]
                box = (x - a.zoom, y - a.zoom, x + a.zoom, y + a.zoom)
            plot(track, a.png, a.real, [('run', log, res)], box,
                 ' '.join('%s=%.3g' % kv for kv in cfg.items() if DEFAULTS.get(kv[0]) != kv[1]) or 'defaults')
        return
    rnd = random.Random(a.seed)
    best_cfg, best = dict(cfg), drive(model, track, cfg)
    print('start: %.1f s %s' % (best['time'], 'HIT at %.0f' % best['at'] if best['hit'] else ''))
    for it in range(a.iters):
        c = dict(best_cfg)
        for k in rnd.sample(list(RANGES), rnd.randint(1, 3)):
            lo, hi = RANGES[k]
            c[k] = min(hi, max(lo, c[k] + rnd.gauss(0, (hi - lo) * (0.25 if it < a.iters / 2 else 0.1))))
            if k == 'points':
                c[k] = round(c[k])
        res = drive(model, track, c)
        if res['time'] < best['time']:
            best, best_cfg = res, c
            print('%4d: %.1f s %s  %s' % (it, res['time'], 'HIT' if res['hit'] else 'ok',
                                         ' '.join('%s=%.3g' % kv for kv in c.items())), flush=True)
    print('best: %.1f s, settings: %s' % (best['time'], ' '.join(
        '--ao2_autopilot_%s=%.3g' % (('drive_' if k not in ('steer', 'gas') else '') + k, v) for k, v in best_cfg.items())))


if __name__ == '__main__':
    main()
