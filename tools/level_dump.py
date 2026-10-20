"""Terrain heights and floating obstacles (buoys, physics props) of a running game, merged over time - the level
streams in and out, so a drive down the river is watched until the game exits or Ctrl+C.

    python tools/level_dump.py <pid> [--out work/level] [--every 8]

Writes <out>.npz (terrains: <key>_z heights, <key>_holes, <key>_geo = x0 y0 step_x step_y) and <out>.json (obstacles:
name, mesh, x, y, z, radius - the component's bounding sphere). Terrain as terrain_dump.py.
"""
import argparse
import json
import struct
import time

import numpy as np

import ue3net
from ue3class import Reflect


def prop_offset(r, cls, name):
    return next(off for off, n, t, p, _ in r.props(r.classes[cls]) if n == name)


def snapshot(pid, terrains, obstacles):
    r = Reflect(pid)
    m = r.m
    off = {n: prop_offset(r, c, n) for c, n in (('Actor', 'Location'), ('Actor', 'DrawScale'), ('Actor', 'DrawScale3D'),
                                                 ('Terrain', 'NumVerticesX'), ('Terrain', 'NumVerticesY'),
                                                 ('Terrain', 'Heights'), ('Terrain', 'InfoData'),
                                                 ('PrimitiveComponent', 'Bounds'))}
    for o in r.objs:
        cls = r.cls(o)
        name = r.oname(o)
        if name.startswith('Default__'):
            continue
        if cls == 'Terrain':
            loc = struct.unpack('>3f', m.read(o + off['Location'], 12))
            key = '%s_%d_%d' % (name, loc[0], loc[1])
            if key in terrains:
                continue
            ds = struct.unpack('>f', m.read(o + off['DrawScale'], 4))[0]
            ds3 = struct.unpack('>3f', m.read(o + off['DrawScale3D'], 12))
            nx, ny = m.u32(o + off['NumVerticesX']), m.u32(o + off['NumVerticesY'])
            hd, hc = m.u32(o + off['Heights']), m.u32(o + off['Heights'] + 4)
            idata, ic = m.u32(o + off['InfoData']), m.u32(o + off['InfoData'] + 4)
            if not hd or hc != nx * ny:
                continue
            h = np.frombuffer(m.read(hd, hc * 2), dtype='>u2').astype(np.float32).reshape(ny, nx)
            holes = np.zeros((ny, nx), bool)
            if idata and ic == nx * ny:
                holes = (np.frombuffer(m.read(idata, ic), dtype=np.uint8).reshape(ny, nx) & 1).astype(bool)
            terrains[key] = (loc[2] + (h - 32768.0) * ds3[2] * ds / 128.0, holes,
                             np.array([loc[0], loc[1], ds3[0] * ds, ds3[1] * ds]))
            print('terrain %s: %dx%d' % (key, nx, ny), flush=True)
        elif cls == 'StaticMeshComponent':
            owner = m.u32(o + 76)
            if not owner or r.cls(owner) not in ('AO2KActor', 'KActor'):
                continue
            sm = m.u32(o + 680)
            mesh = r.oname(sm) if sm else '-'
            b = struct.unpack('>7f', m.read(o + off['Bounds'], 28))  # Origin, BoxExtent, SphereRadius
            oname = r.oname(owner)
            if oname not in obstacles:
                print('obstacle %s %s at (%.0f %.0f %.0f) r %.0f' % (oname, mesh, b[0], b[1], b[2], b[6]), flush=True)
            obstacles[oname] = dict(name=oname, mesh=mesh, x=b[0], y=b[1], z=b[2], radius=b[6],
                                    extent=[b[3], b[4], b[5]])


def save(out, terrains, obstacles):
    arrays = {}
    for i, (key, (z, holes, geo)) in enumerate(sorted(terrains.items())):
        arrays['t%d_z' % i], arrays['t%d_holes' % i], arrays['t%d_geo' % i] = z, holes, geo
    np.savez_compressed(out + '.npz', **arrays)
    with open(out + '.json', 'w') as f:
        json.dump(sorted(obstacles.values(), key=lambda o: o['name']), f, indent=0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pid', type=int)
    ap.add_argument('--out', default='work/level')
    ap.add_argument('--every', type=float, default=8.0)
    a = ap.parse_args()
    terrains, obstacles = {}, {}
    try:
        while True:
            try:
                snapshot(a.pid, terrains, obstacles)
            except (SystemExit, OSError, struct.error, ValueError) as e:
                print('snapshot failed: %s' % e, flush=True)
                if not ue3net_alive(a.pid):
                    break
            save(a.out, terrains, obstacles)
            time.sleep(a.every)
    except KeyboardInterrupt:
        pass
    save(a.out, terrains, obstacles)
    print('%d terrains, %d obstacles -> %s.npz/.json' % (len(terrains), len(obstacles), a.out))


def ue3net_alive(pid):
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)
    if not h:
        return False
    code = ctypes.c_ulong()
    ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(h)
    return code.value == 259


if __name__ == '__main__':
    main()
