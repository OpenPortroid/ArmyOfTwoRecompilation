"""Compare the sync_trace.log of two co-op machines (ao2_coop_sync_trace).

    python tools/sync_diff.py <host sync_trace.log> <partner sync_trace.log> [--context N]

Finds the same game in both logs (the same checksum on its first frame; a log
can hold several games) and prints the first lockstep frame where anything
differs: every pawn (P lines), the players (F lines), the rand() callers (R
lines), the rand() state and the checksum - then the lines around it. The
checksum (cs) only follows the cause: rand() at the tick start XORed into
+44, the physics hash into +46.
"""
import argparse
import re
from collections import Counter


class Game:
    def __init__(self):
        self.frames = {}  # frame -> F line
        self.sites = {}   # frame -> Counter of rand() callers
        self.pawns = {}   # frame -> {pawn name: fields string}
        self.order = []


def parse_pawns(rest):
    """' name loc a b c rot .. hp n name2 ...' -> {name: 'loc a b c ...'}."""
    out, name, fields = {}, None, []
    keys = {"loc", "rot", "vel", "hp"}
    tokens = rest.split()
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t in keys:
            n = 2 if t == "hp" else 4
            fields.append(" ".join(tokens[i:i + n]))
            i += n
            continue
        if name is not None:
            out[name] = " ".join(fields)
        name, fields = t, []
        i += 1
    if name is not None:
        out[name] = " ".join(fields)
    return out


def load(path):
    games, g = [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("F "):
                n = int(line.split()[1])
                if g is None or n in g.frames:
                    g = Game()
                    games.append(g)
                g.frames[n] = line
                g.order.append(n)
            elif g is None:
                continue
            elif line.startswith("R "):
                parts = line.split()
                try:
                    g.sites[int(parts[1])] = Counter(
                        {p.rsplit("=", 1)[0]: int(p.rsplit("=", 1)[1]) for p in parts[2:]})
                except (ValueError, IndexError):
                    pass  # the last line of a log still being written
            elif line.startswith("P "):
                parts = line.split(" ", 2)
                g.pawns[int(parts[1])] = parse_pawns(parts[2] if len(parts) > 2 else "")
    return games


def fields(line):
    m = re.match(r"F (\d+) cs (\w+) (\w+) rand (\S+) calls (\d+) draw (\d+) \|(.*)", line)
    if not m:
        return None
    return {"cs": (m.group(2), m.group(3)), "rand": m.group(4), "calls": int(m.group(5)), "players": m.group(7)}


def first_cs(g):
    return g.frames[g.order[0]].split()[3] if g.order else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--context", type=int, default=3)
    ap.add_argument("--cs", help="pick the game whose first frame has this checksum (default: the longest)")
    args = ap.parse_args()
    ga, gb = load(args.a), load(args.b)
    pairs = [(a, b) for a in ga for b in gb if first_cs(a) and first_cs(a) == first_cs(b)
             and (not args.cs or first_cs(a).upper() == args.cs.upper())]
    if not pairs:
        print("no game found in both logs")
        return
    A, B = max(pairs, key=lambda p: len(set(p[0].frames) & set(p[1].frames)))
    # The last common frame may still be half written (a running game).
    common = sorted(set(A.frames) & set(B.frames))[:-1]
    print(f"games: A {len(ga)}, B {len(gb)}; the longest common one: "
          f"A {A.order[0]}..{A.order[-1]}, B {B.order[0]}..{B.order[-1]}, common {len(common)} frames")
    first = {}
    pawn_diff = None
    for n in common:
        x, y = fields(A.frames[n]), fields(B.frames[n])
        if not x or not y:
            continue
        for key in ("players", "calls", "rand", "cs"):
            if key not in first and x[key] != y[key]:
                first[key] = n
        if "sites" not in first and A.sites.get(n, Counter()) != B.sites.get(n, Counter()):
            first["sites"] = n
        if "pawns" not in first and n in A.pawns and n in B.pawns and A.pawns[n] != B.pawns[n]:
            first["pawns"] = n
            pa, pb = A.pawns[n], B.pawns[n]
            pawn_diff = []
            for name in sorted(set(pa) | set(pb)):
                if pa.get(name) != pb.get(name):
                    pawn_diff.append((name, pa.get(name, "(none)"), pb.get(name, "(none)")))
        if len(first) == 6:
            break
    if not first:
        print("no difference in the common frames")
        return
    for key in ("pawns", "players", "sites", "calls", "rand", "cs"):
        print(f"first difference in {key:8}: {first.get(key, '-')}")
    if pawn_diff:
        print(f"\n=== pawns that differ on frame {first['pawns']} (A / B) ===")
        for name, a, b in pawn_diff:
            print(f"  {name}\n    A {a}\n    B {b}")
    start = min(first.values())
    print(f"\n=== around frame {start} ===")
    for n in common:
        if start - args.context <= n <= start + args.context:
            print(f"A {A.frames[n]}")
            print(f"B {B.frames[n]}")
    if "sites" in first:
        n = first["sites"]
        a, b = A.sites.get(n, Counter()), B.sites.get(n, Counter())
        print(f"\n=== rand() callers of frame {n} that differ (A / B) ===")
        for site in sorted(set(a) | set(b)):
            if a[site] != b[site]:
                print(f"  {site}: {a[site]} / {b[site]}")


if __name__ == "__main__":
    main()
