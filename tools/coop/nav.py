"""Steering for tools/coop/auto_coop.sh, from the last frame of a sync_trace.log.

    python nav.py <sync_trace.log> <player id 0|1> [route file [progress file]]

Target: the nearest live enemy ("iraqi" pawns) when one is within 1500 units
or the route is done, else the route (points "x y z" per line, recorded from
a player's walk): the point after the last one reached (within 150 units,
height included; kept in the progress file, else the nearest one). Prints the mouse counts to
face the target (mx) and the distance to it."""
import math
import struct
import sys

path, pid = sys.argv[1], int(sys.argv[2])
route = []
if len(sys.argv) > 3:
    route = [[float(v) for v in l.split()] for l in open(sys.argv[3]) if l.strip()]
with open(path, "rb") as fh:
    fh.seek(0, 2)
    fh.seek(max(0, fh.tell() - 262144))
    lines = fh.read().decode("utf-8", "replace").splitlines()[1:]
F = next(l for l in reversed(lines[:-2]) if l.startswith("F "))
P = next(l for l in reversed(lines[:-2]) if l.startswith("P "))
f = lambda h: struct.unpack(">f", bytes.fromhex(h))[0]
t = F.split()
i = t.index(f"p{pid}")
look_yaw = f(t[i + 3])
j = t.index("loc", i)
me = [f(x) for x in t[j + 1:j + 4]]
p = P.split()
enemy = None
for k, tok in enumerate(p):
    if "iraqi" in tok and k + 1 < len(p) and p[k + 1] == "loc":
        loc = [f(x) for x in p[k + 2:k + 5]]
        if int(p[p.index("hp", k) + 1]) <= 0:
            continue
        d = math.dist(me[:2], loc[:2])
        if enemy is None or d < enemy[0]:
            enemy = (d, tok, loc)
mode, target = "none", None
state = sys.argv[4] if len(sys.argv) > 4 else None
if route:
    try:
        near = int(open(state).read())
    except (OSError, ValueError, TypeError):
        near = min(range(len(route)), key=lambda n: math.dist(me, route[n]))
    while near < len(route) - 1 and math.dist(me, route[near + 1]) < 150:
        near += 1
    if state:
        open(state, "w").write(str(near))
    if near < len(route) - 1:
        mode, target = f"route {near}/{len(route)}", route[near + 1]
if enemy and (enemy[0] < 1500 or target is None):
    mode, target = f"enemy {enemy[1]}", enemy[2]
if target is None:
    print(f"frame {t[1]} me {me[0]:.0f} {me[1]:.0f} no target")
    sys.exit()
d = math.dist(me[:2], target[:2])
yaw = math.degrees(math.atan2(target[1] - me[1], target[0] - me[0])) * 65536 / 360
delta = (yaw - look_yaw + 32768) % 65536 - 32768
# Measured in game: mx500 turned the look by ~7750 units (settings sensitivity).
units_per_count = 15.5
print(f"frame {t[1]} me {me[0]:.0f} {me[1]:.0f} {me[2]:.0f} pos {int(me[0])},{int(me[1])} {mode} dist {int(d)} turn {delta:.0f} "
      f"mx {round(delta / units_per_count)}")
