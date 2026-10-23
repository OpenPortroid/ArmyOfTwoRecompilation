"""The hovercraft's motion fitted from the autopilot's telemetry (drive_telemetry.csv, ao2_autopilot_drive_telemetry).

    python tools/boat_model.py fit run1.csv [run2.csv ...] [--out work/boat_model.json]
    python tools/boat_model.py check model.json run.csv [--horizon 3]   open-loop error from real states, real inputs

Body frame: u = speed forward (along the heading), v = sideways (+ = to the right), r = turn rate (rad/s, + = right:
the game's yaw grows clockwise seen from above, yaw 0 = +X, +Y to its right). Inputs: steer -1..1, gas 0..1,
brake 0/1. The model (parameters in PARAMS):
    u' = accel * gas - drag * u - brake_force * brake          u capped at umax (the game's: 1504)
    r' = (turn * steer * (1 + turn_speed * u / umax) - r) / turn_lag
    v' = -side_drag * v - slip * u * r
Fitted by least squares on the simulated track of 1 s windows (positions, heading, speed) - not on differentiated
data, which is far too noisy at 30 Hz. Windows with a hit (a bank: the speed drops sharply), a pause (cutscene,
nothing moves) or backing up are left out.
"""
import argparse
import csv
import json
import math

import numpy as np
from scipy.optimize import least_squares

PARAMS = ['accel', 'drag', 'brake_force', 'turn', 'turn_speed', 'turn_lag', 'side_drag', 'slip']
START = [900.0, 0.5, 1500.0, 1.5, 0.0, 0.25, 2.0, 0.3]
LOWER = [10.0, 0.0, 0.0, 0.05, -0.9, 0.02, 0.0, -2.0]
UPPER = [10000.0, 10.0, 20000.0, 10.0, 5.0, 3.0, 50.0, 2.0]
UMAX = 1504.0
WINDOW = 1.0


def load(path):
    with open(path) as f:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(f)]


def body(rows):
    """Per row: t, x, y, yaw (unwrapped), u, v, inputs; r from the yaw's change (central difference)."""
    out, prev = [], None
    for row in rows:
        yaw = row['yaw']
        if prev is not None:
            yaw = prev['yaw'] + math.remainder(yaw - prev['yaw'], 2 * math.pi)
        c, s = math.cos(yaw), math.sin(yaw)
        rec = dict(t=row['t'], x=row['x'], y=row['y'], yaw=yaw, u=row['vx'] * c + row['vy'] * s,
                   v=-row['vx'] * s + row['vy'] * c, steer=row['steer'], gas=row['gas'], brake=row['brake'])
        out.append(rec)
        prev = rec
    for i, rec in enumerate(out):
        a, b = out[max(i - 1, 0)], out[min(i + 1, len(out) - 1)]
        rec['r'] = (b['yaw'] - a['yaw']) / (b['t'] - a['t']) if b['t'] > a['t'] else 0.0
    return out


def step(p, st, steer, gas, brake, dt):
    accel, drag, brake_force, turn, turn_speed, turn_lag, side_drag, slip = p
    u, v, r = st['u'], st['v'], st['r']
    du = accel * gas - drag * u - brake_force * brake
    dr = (turn * steer * (1 + turn_speed * u / UMAX) - r) / turn_lag
    dv = -side_drag * v - slip * u * r
    u = min(u + du * dt, UMAX)
    r += dr * dt
    v += dv * dt
    yaw = st['yaw'] + r * dt
    c, s = math.cos(yaw), math.sin(yaw)
    return dict(x=st['x'] + (u * c - v * s) * dt, y=st['y'] + (u * s + v * c) * dt, yaw=yaw, u=u, v=v, r=r)


def windows(recs, length=WINDOW, every=0.5):
    """Index ranges [i, j] of clean stretches `length` s long, starting every `every` s."""
    out, i = [], 0
    while i < len(recs):
        a = recs[i]
        j, ok = i, True
        while j + 1 < len(recs) and recs[j + 1]['t'] - a['t'] <= length:
            p, q = recs[j], recs[j + 1]
            dt = q['t'] - p['t']
            moved = math.hypot(q['x'] - p['x'], q['y'] - p['y'])
            speed_p, speed_q = math.hypot(p['u'], p['v']), math.hypot(q['u'], q['v'])
            if dt <= 0 or dt > 0.1 or p['brake'] or (moved == 0 and speed_p > 50) or \
                    speed_q < speed_p - 2500 * dt - 60:  # a hit: slower than any drag would make it
                ok = False
                break
            j += 1
        if ok and recs[j]['t'] - a['t'] > length * 0.9:
            out.append((i, j))
        while i < len(recs) and recs[i]['t'] - a['t'] < every:
            i += 1
    return out


def simulate(p, recs, i, j):
    a = recs[i]
    st = {k: a[k] for k in ('x', 'y', 'yaw', 'u', 'v', 'r')}
    track = [st]
    for k in range(i, j):
        st = step(p, st, recs[k]['steer'], recs[k]['gas'], recs[k]['brake'], recs[k + 1]['t'] - recs[k]['t'])
        track.append(st)
    return track


def residuals(p, runs):
    res = []
    for recs, wins in runs:
        for i, j in wins:
            track = simulate(p, recs, i, j)
            for k in range(1, j - i + 1, 3):
                real, sim = recs[i + k], track[k]
                res += [(sim['x'] - real['x']) / 100.0, (sim['y'] - real['y']) / 100.0,
                        math.remainder(sim['yaw'] - real['yaw'], 2 * math.pi) * 3.0,
                        (sim['u'] - real['u']) / 300.0]
    return res


def fit(paths):
    runs = []
    for path in paths:
        recs = body(load(path))
        wins = windows(recs)
        runs.append((recs, wins))
        print('%s: %d rows, %d clean windows of %.0f s' % (path, len(recs), len(wins), WINDOW))
    sol = least_squares(residuals, START, bounds=(LOWER, UPPER), args=(runs,), x_scale='jac')
    model = dict(zip(PARAMS, map(float, sol.x)), umax=UMAX)
    print('fit: cost %.1f -> %s' % (sol.cost, ', '.join('%s %.4g' % kv for kv in model.items())))
    return model


def check(model, path, horizon=3.0):
    """Every 2 s of the run: simulate `horizon` s with the recorded inputs; the position error at the end."""
    p = [model[k] for k in PARAMS]
    recs = body(load(path))
    errs = sorted(math.hypot(simulate(p, recs, i, j)[-1]['x'] - recs[j]['x'],
                             simulate(p, recs, i, j)[-1]['y'] - recs[j]['y'])
                  for i, j in windows(recs, horizon, 2.0))
    if errs:
        print('%s: %d windows of %.0f s, position error median %.0f, p90 %.0f, max %.0f units' % (
            path, len(errs), horizon, errs[len(errs) // 2], errs[int(len(errs) * 0.9)], errs[-1]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('fit')
    p.add_argument('csv', nargs='+')
    p.add_argument('--out', default='work/boat_model.json')
    p = sub.add_parser('check')
    p.add_argument('model')
    p.add_argument('csv', nargs='+')
    p.add_argument('--horizon', type=float, default=3.0)
    a = ap.parse_args()
    if a.cmd == 'fit':
        model = fit(a.csv)
        with open(a.out, 'w') as f:
            json.dump(model, f, indent=1)
        print('->', a.out)
    else:
        with open(a.model) as f:
            model = json.load(f)
        for path in a.csv:
            for h in (1.0, a.horizon):
                check(model, path, h)


if __name__ == '__main__':
    main()
