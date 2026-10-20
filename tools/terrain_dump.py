"""The level's terrain heights from a running game (guest memory), for tools/boat_sim.py's water map.

    python tools/terrain_dump.py <pid> [--out work/terrain.npz]

Terrain (UE3): Location +228, DrawScale +? (from reflection), DrawScale3D, NumVerticesX/Y; Heights (TArray of WORD at
+452: data, count) - world z = Location.z + (h - 32768) * DrawScale3D.z * DrawScale / 128; vertex (i, j) at
Location + (i * DrawScale3D.x, j * DrawScale3D.y) * DrawScale. InfoData (TArray of BYTE, +464): bit 0 = a hole.
"""
import argparse
import struct
import sys

import numpy as np

from ue3class import Reflect


def prop_offset(r, cls, name):
    return next(off for off, n, t, p, _ in r.props(r.classes[cls]) if n == name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pid', type=int)
    ap.add_argument('--out', default='work/terrain.npz')
    a = ap.parse_args()
    r = Reflect(a.pid)
    m = r.m
    off = {n: prop_offset(r, c, n) for c, n in (('Actor', 'Location'), ('Actor', 'DrawScale'), ('Actor', 'DrawScale3D'),
                                                 ('Terrain', 'NumVerticesX'), ('Terrain', 'NumVerticesY'),
                                                 ('Terrain', 'Heights'), ('Terrain', 'InfoData'))}
    out = {}
    n = 0
    for o in r.objs:
        if r.cls(o) != 'Terrain' or r.oname(o).startswith('Default__'):
            continue
        loc = struct.unpack('>3f', m.read(o + off['Location'], 12))
        ds = struct.unpack('>f', m.read(o + off['DrawScale'], 4))[0]
        ds3 = struct.unpack('>3f', m.read(o + off['DrawScale3D'], 12))
        nx, ny = m.u32(o + off['NumVerticesX']), m.u32(o + off['NumVerticesY'])
        hd, hc = m.u32(o + off['Heights']), m.u32(o + off['Heights'] + 4)
        idata, ic = m.u32(o + off['InfoData']), m.u32(o + off['InfoData'] + 4)
        if not hd or hc != nx * ny:
            print('%s: heights %d for %dx%d - skipped' % (r.oname(o), hc, nx, ny))
            continue
        h = np.frombuffer(m.read(hd, hc * 2), dtype='>u2').astype(np.float32).reshape(ny, nx)
        z = loc[2] + (h - 32768.0) * ds3[2] * ds / 128.0
        holes = np.zeros((ny, nx), bool)
        if idata and ic == nx * ny:
            holes = (np.frombuffer(m.read(idata, ic), dtype=np.uint8).reshape(ny, nx) & 1).astype(bool)
        key = 't%d' % n
        out[key + '_z'] = z
        out[key + '_holes'] = holes
        out[key + '_geo'] = np.array([loc[0], loc[1], ds3[0] * ds, ds3[1] * ds])
        print('%s: %dx%d at (%.0f %.0f), step %.0f x %.0f, z %.0f..%.0f, holes %d' % (
            r.oname(o), nx, ny, loc[0], loc[1], ds3[0] * ds, ds3[1] * ds, z.min(), z.max(), holes.sum()))
        n += 1
    np.savez_compressed(a.out, **out)
    print('%d terrains -> %s' % (n, a.out))


if __name__ == '__main__':
    sys.exit(main())
