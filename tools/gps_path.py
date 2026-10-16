"""The GPS route of the current level (the orange line with arrows shown while Back / Tab is held), from guest memory.

    python tools/gps_path.py <pid | A | B> [--json out.json] [--png out.png]

The line is not computed by the game: level designers placed ribbon static meshes (StaticMeshActor, material
GPS_arrows / GPS_arrow_mat, e.g. 01_training_arrow, 05_oil_usbase_arrow), shown in the GPS view (PrimitiveComponent.bVisibleInGPS) and
switched by Kismet as the level goes on. The blue outlines are another mesh (GPS_blue_marker, e.g. 01_training_gps).
A ribbon is quads across the route: UV u runs along it (the arrows scroll along u), v = 0..1 across. The route is the
middle of each cross-section (the two vertices with the same u), ordered by u; each connected piece of the mesh is a
separate polyline.

UStaticMesh (not reflected, this build): LODModels TIndirectArray at +0x3C -> FStaticMeshRenderData: vertices at
+0x20 (data, stride +0x24: 40 or 48; float3 position, ..., float2 uv at +32), count +0x28; u16 indices at +0x7C, count +0x80;
elements (data, count) at +0xAC, element +0 = material. StaticMeshComponent: StaticMesh +680, LocalToWorld +112
(row vectors), HiddenGame +448, Owner +76. Actor: Location +228, bHidden +112.
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ue3class import Reflect  # noqa: E402

ROUTE_MATERIALS = ('GPS_arrows', 'GPS_arrow_mat')  # training / missions
STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'coop', 'local', 'aot_state.json')


def mask(r, cls, prop):
    return next(r.m.u32(p + 0x80) for off, n, t, p, _ in r.props(r.classes[cls]) if n == prop)


def is_a(r, o, cname):
    c = r.m.u32(o + 52)
    for _ in range(40):
        if not c:
            return False
        if r.oname(c) == cname:
            return True
        c = r.m.u32(c + 60)
    return False


def mesh_data(r, sm):
    m = r.m
    lods = m.u32(sm + 0x3C)
    lod = m.u32(lods) if lods else 0
    if not lod:
        return None
    vdata, stride, nv = m.u32(lod + 0x20), m.u32(lod + 0x24), m.u32(lod + 0x28)
    idata, ni = m.u32(lod + 0x7C), m.u32(lod + 0x80)
    edata, ne = m.u32(lod + 0xAC), m.u32(lod + 0xB0)
    if not (vdata and idata and 40 <= stride <= 64 and 0 < nv < 200000 and 0 < ni < 600000 and edata and 0 < ne < 64):
        return None
    vb = m.read(vdata, stride * nv)
    pos = [struct.unpack_from('>3f', vb, i * stride) for i in range(nv)]
    uv = [struct.unpack_from('>2f', vb, i * stride + 32) for i in range(nv)]
    idx = struct.unpack('>%dH' % ni, m.read(idata, ni * 2))
    mats = [r.oname(m.u32(edata + i * 0x40)) for i in range(ne)]
    return pos, uv, idx, mats


def centerlines(pos, uv, idx):
    """Connected pieces of the ribbon -> [[(x, y, z, u)...]] ordered by u."""
    parent = list(range(len(pos)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for t in range(0, len(idx) - 2, 3):
        a, b, c = find(idx[t]), find(idx[t + 1]), find(idx[t + 2])
        parent[b] = a
        parent[find(c)] = a
    pieces = {}
    for i in set(idx):
        pieces.setdefault(find(i), []).append(i)
    lines = []
    for verts in pieces.values():
        verts.sort(key=lambda i: uv[i][0])
        groups, cur = [], [verts[0]]
        for i in verts[1:]:
            if uv[i][0] - uv[cur[0]][0] < 0.01:
                cur.append(i)
            else:
                groups.append(cur)
                cur = [i]
        groups.append(cur)
        line = []
        for g in groups:
            n = len(g)
            line.append(tuple(sum(pos[i][k] for i in g) / n for k in range(3)) + (uv[g[0]][0],))
        if len(line) >= 2:
            lines.append(line)
    return lines


def transform(p, l2w):
    x, y, z = p[:3]
    return (x * l2w[0] + y * l2w[4] + z * l2w[8] + l2w[12],
            x * l2w[1] + y * l2w[5] + z * l2w[9] + l2w[13],
            x * l2w[2] + y * l2w[6] + z * l2w[10] + l2w[14]) + tuple(p[3:])


def read_route(pid):
    r = Reflect(pid)
    m = r.m
    hidden_game = mask(r, 'PrimitiveComponent', 'HiddenGame')
    in_gps = mask(r, 'PrimitiveComponent', 'bVisibleInGPS')
    actor_hidden = mask(r, 'Actor', 'bHidden')
    meshes = []
    for o in r.objs:
        if r.cls(o) != 'StaticMeshComponent' or r.oname(o).startswith('Default__'):
            continue
        sm = m.u32(o + 680)
        if not sm:
            continue
        data = mesh_data(r, sm)
        if not data or not any(mt in ROUTE_MATERIALS for mt in data[3]):
            continue
        pos, uv, idx, mats = data
        flags = m.u32(o + 448)
        owner = m.u32(o + 76)
        l2w = struct.unpack('>16f', m.read(o + 112, 64))
        meshes.append({
            'component': '%08X' % o,
            'mesh': r.oname(sm),
            'owner': r.oname(owner),
            'owner_addr': '%08X' % owner,
            'materials': mats,
            'hidden': bool(flags & hidden_game) or bool(owner and m.u32(owner + 112) & actor_hidden),
            'in_gps': bool(flags & in_gps),
            'lines': [[[round(c, 1) for c in transform(p, l2w)[:3]] + [round(p[3], 3)] for p in line]
                      for line in centerlines(pos, uv, idx)],
        })
    controller = next((o for o in r.objs if r.cls(o) == 'AO2PlayerController' and m.u32(o + 452)
                       and not r.oname(o).startswith('Default__')), 0)
    route = {'player': None, 'meshes': meshes, 'controller': '%08X' % controller,
             'masks': [hidden_game, actor_hidden], 'mem': m, 'pid': pid}
    refresh(route)
    return route


def refresh(route):
    """Re-reads what changes while the level runs: which route pieces are shown and where the player is (a few reads,
    no object scan). Returns the hidden flags of the pieces (compare to see when the level showed or hid one)."""
    m = route['mem']
    hidden_game, actor_hidden = route['masks']
    for mesh in route['meshes']:
        flags = m.u32(int(mesh['component'], 16) + 448)
        owner = int(mesh['owner_addr'], 16)
        mesh['hidden'] = bool((flags or 0) & hidden_game) or bool(owner and (m.u32(owner + 112) or 0) & actor_hidden)
    controller = int(route['controller'], 16)
    pawn = (m.u32(controller + 452) or 0) if controller else 0
    raw = m.read(pawn + 228, 12) if pawn else None
    route['player'] = [round(v, 1) for v in struct.unpack('>3f', raw)] if raw else None
    if not raw:
        # Dead / respawned / reloaded: the controller may be another object now.
        import menustate
        st = route.setdefault('_menustate', menustate.State(route['pid']))
        route['controller'] = '%08X' % (menustate.player(st) or 0)
    return tuple(mesh['hidden'] for mesh in route['meshes'])


ROUTES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'routes')


def load_path(file):
    """Points of an autopilot.path-style file (x y z per line, '#' comments)."""
    pts = []
    with open(file) as f:
        for line in f:
            v = line.split('#')[0].split()
            if len(v) >= 3:
                pts.append([float(v[0]), float(v[1]), float(v[2])])
    return pts


def length(path):
    return sum(((path[i + 1][0] - path[i][0]) ** 2 + (path[i + 1][1] - path[i][1]) ** 2) ** 0.5
               for i in range(len(path) - 1))


def from_nearest(path, here):
    """The rest of `path` from its point nearest to `here`, and that distance."""
    k = min(range(len(path)), key=lambda j: (path[j][0] - here[0]) ** 2 + (path[j][1] - here[1]) ** 2)
    return path[k:], ((path[k][0] - here[0]) ** 2 + (path[k][1] - here[1]) ** 2) ** 0.5


def pieces(pts, jump=2000.0):
    """A recording split where the player jumped (a load, a respawn, a cutscene that moved him)."""
    out, cur = [], pts[:1]
    for p in pts[1:]:
        if (p[0] - cur[-1][0]) ** 2 + (p[1] - cur[-1][1]) ** 2 > jump ** 2:
            out.append(cur)
            cur = []
        cur.append(p)
    out.append(cur)
    return [c for c in out if len(c) >= 2]


def recorded_route(here, near=1500.0, exclude=()):
    """A piece of a recorded route (tools/routes/*.path, aot.py record; pieces split at jumps) passing within `near`
    of `here` with more than a few metres still ahead: (name, rest of it) of the closest one, or (None, [])."""
    best = (None, [], near)
    if here and os.path.isdir(ROUTES):
        for fn in sorted(os.listdir(ROUTES)):
            if not fn.endswith('.path'):
                continue
            for k, piece in enumerate(pieces(load_path(os.path.join(ROUTES, fn)))):
                name = '%s#%d' % (fn[:-5], k + 1)
                if name in exclude:
                    continue
                rest, d = from_nearest(piece, here)
                if d < best[2] and len(rest) >= 2 and length(rest) > 300:
                    best = (name, rest, d)
    return best[0], best[1]


def chain(route, visible_only=False, join=1500.0):
    """One polyline to run, from the piece nearest to the player on: each piece is run towards lower u (the way its
    arrows point), the next piece is the unused one starting nearest to the end of the last (pieces join end to
    start; u restarts at each join, so it cannot order pieces). The other branch of a fork is left out."""
    pieces = [list(reversed(line)) for mesh in route['meshes'] if mesh['in_gps'] and not (visible_only and mesh['hidden'])
              for line in mesh['lines']]
    if not pieces:
        return []

    def d2(a, b):
        return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2

    here = route['player'] or pieces[0][0]
    k, start = min(((k, j) for k, piece in enumerate(pieces) for j in range(len(piece))),
                   key=lambda kj: d2(pieces[kj[0]][kj[1]], here))
    path = pieces[k][start:]
    used = {k}
    while True:
        end = path[-1]
        nxt = min((k for k in range(len(pieces)) if k not in used), key=lambda k: d2(pieces[k][0], end), default=None)
        if nxt is None or d2(pieces[nxt][0], end) > join ** 2:
            break
        used.add(nxt)
        path += pieces[nxt][1:] if d2(pieces[nxt][0], end) < 1 else pieces[nxt]
    return [p[:3] for p in path]


ACTIONS = os.path.join(ROUTES, 'actions.txt')


def write_actions(workdir):
    """Action points (tools/routes/actions.txt, `aot.py mark`: "x y z button  # note") to the game: autopilot.actions."""
    if os.path.exists(ACTIONS):
        tmp = os.path.join(workdir, 'autopilot.actions.tmp')
        with open(ACTIONS) as src, open(tmp, 'w') as f:
            f.write(src.read())
        os.replace(tmp, os.path.join(workdir, 'autopilot.actions'))


def write_path(workdir, path):
    """Starts the autopilot (autopilot.cpp) on `path`: autopilot.path in the game's working directory. Returns the
    route length."""
    write_actions(workdir)
    tmp = os.path.join(workdir, 'autopilot.path.tmp')
    with open(tmp, 'w') as f:
        f.write('# GPS route, %d points\n' % len(path))
        f.writelines('%.1f %.1f %.1f\n' % tuple(p) for p in path)
    os.replace(tmp, os.path.join(workdir, 'autopilot.path'))
    return length(path)


def plot(route, out):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(10, 10))
    colors = ['tab:orange', 'tab:red', 'tab:purple', 'tab:brown', 'tab:pink', 'tab:olive']
    for k, mesh in enumerate(route['meshes']):
        col = colors[k % len(colors)]
        for j, line in enumerate(mesh['lines']):
            xs, ys = [p[0] for p in line], [p[1] for p in line]
            ax.plot(xs, ys, '-', color=col, lw=1.5, alpha=0.4 if mesh['hidden'] else 1,
                    label='%s%s' % (mesh['mesh'], ' (hidden)' if mesh['hidden'] else '') if j == 0 else None)
            ax.plot(xs[0], ys[0], 'o', color=col, ms=4)  # lowest u
            ax.annotate('u%.0f' % line[0][3], (xs[0], ys[0]), fontsize=6)
    path = chain(route)
    if path:
        ax.plot([p[0] for p in path], [p[1] for p in path], 'k:', lw=1, label='autopilot route')
    if route['player']:
        ax.plot(route['player'][0], route['player'][1], 'b*', ms=14, label='player')
    ax.set_aspect('equal')
    ax.invert_yaxis()  # UE3: +Y is to the right of +X; seen from above with X right, Y goes down
    ax.legend(fontsize=7)
    ax.set_title('GPS route (dot = lowest u)')
    fig.savefig(out, dpi=110, bbox_inches='tight')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', help='pid, or A / B (tools/aot.py instance)')
    ap.add_argument('--json')
    ap.add_argument('--png')
    ap.add_argument('--run', action='store_true',
                    help='start the autopilot (autopilot.path in the game\'s working directory)')
    ap.add_argument('--stop', action='store_true', help='stop the autopilot')
    ap.add_argument('--all', action='store_true', help='--run: the whole route, also the pieces not shown yet')
    a = ap.parse_args()
    if a.target.isdigit():
        pid, workdir = int(a.target), None
    else:
        with open(STATE) as f:
            pid = json.load(f)[a.target]
        workdir = os.path.join(os.path.dirname(STATE), a.target)
    if a.stop:
        with open(os.path.join(workdir or '.', 'autopilot.path'), 'w') as f:
            f.write('stop\n')
        return
    route = read_route(pid)
    print('player', route['player'])
    for mesh in route['meshes']:
        print('%s %s owner=%s hidden=%d in_gps=%d: %s' % (
            mesh['component'], mesh['mesh'], mesh['owner'], mesh['hidden'], mesh['in_gps'],
            ', '.join('%d pts u%.1f..%.1f' % (len(l), l[0][3], l[-1][3]) for l in mesh['lines'])))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({k: v for k, v in route.items() if k != 'mem' and not k.startswith('_')}, f, indent=1)
    if a.png:
        plot(route, a.png)
    if a.run:
        if not workdir:
            raise SystemExit('--run needs an instance (A / B) to know the working directory')
        path = chain(route, visible_only=not a.all)
        if len(path) < 2:
            raise SystemExit('no route here')
        print('autopilot: %d points, %.0f units' % (len(path), write_path(workdir, path)))

if __name__ == '__main__':
    main()
