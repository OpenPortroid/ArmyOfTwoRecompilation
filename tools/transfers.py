"""Summarizes EDRAM transfers in frame_trace.txt: kind, area, and the draw that caused them.

Usage: python transfers.py [frame_trace.txt]
"""
import collections
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"C:\aot2\aot\out\build\win-amd64-release\frame_trace.txt"
lines = open(path).read().split("\n")
draw = None
kinds = collections.Counter()
area_by_kind = collections.Counter()
by_draw_kind = collections.Counter()
rows = []
for l in lines:
    m = re.match(r"draw (\d+): prim (\d+) count (\d+).*vs (\w+) ps (\w+) \| pitch (\d+) msaa (\d) mode (\d) mask (\w+)", l)
    if m:
        draw = {"n": int(m[1]), "prim": int(m[2]), "vs": m[4][:8], "ps": m[5][:8], "mode": m[8], "mask": m[9]}
        continue
    if l.startswith("resolve:"):
        draw = {"n": -1, "prim": 0, "vs": "resolve", "ps": "", "mode": "", "mask": ""}
        continue
    m = re.match(r"  transfer -> (\w+) base (\d+) fmt (\d+) pitch (\d+) x(\d) <- (\w+) base (\d+) fmt (\d+) pitch (\d+) x(\d) "
                 r"rect (\d+),(\d+)-(\d+),(\d+)", l)
    if m:
        dst = f"{m[1]}{m[3]} p{m[4]}x{m[5]}"
        src = f"{m[6]}{m[8]} p{m[9]}x{m[10]}"
        area = (int(m[13]) - int(m[11])) * (int(m[14]) - int(m[12]))
        kind = f"{m[6]}->{m[1]}"
        kinds[kind] += 1
        area_by_kind[kind] += area * (9 if m[1] == "depth" else 1) * int(m[5])
        cause = f"vs {draw['vs']} ps {draw['ps']} prim {draw['prim']}" if draw else "?"
        by_draw_kind[(kind, cause)] += 1
        rows.append((draw["n"] if draw else -1, dst, src, area, cause))
print("transfers by kind (count, weighted pixel passes = area x samples x 9 for depth):")
for k, c in kinds.most_common():
    print(f"  {k:14s} {c:4d}  {area_by_kind[k] / 1e6:7.2f} Mpx")
print("by kind and causing draw:")
for (k, cause), c in by_draw_kind.most_common(20):
    print(f"  {c:4d}  {k:14s} {cause}")
print("first 40:")
for r in rows[:40]:
    print("  draw %5d  %-22s <- %-22s area %7d  %s" % r)
