"""Compare the sync_dump.bin of two co-op machines (ao2_coop_sync_dump).

    python tools/sync_dump_diff.py <A sync_dump.bin> <B sync_dump.bin> [--before FRAME]

For each enemy object (pawn / mesh / controller) prints the 32-bit words that
were equal on both machines in the first common dump and differ later, by the
frame they first differ (earliest first) - the state that split. Words that
differ from the start (pointers to per-machine allocations) are left out.
"""
import argparse
import struct
from collections import defaultdict

KINDS = {0: "pawn", 1: "mesh", 2: "controller"}


def load(path, game=-1):
    """A file holds every game of the run (frame numbers start again with
    each): game = index of the one to load (-1 = the last one)."""
    games = []
    data = open(path, "rb").read()
    i, prev = 0, None
    while i + 57 <= len(data):
        frame, kind = struct.unpack_from("<IB", data, i)
        if prev is None or frame + 1000 < prev:
            games.append(defaultdict(dict))  # (kind, name) -> frame -> bytes
        prev = frame if prev is None else max(frame, prev) if frame + 1000 >= prev else frame
        out = games[-1]
        name = data[i + 5:i + 53].split(b"\0")[0].decode("ascii", "replace")
        size = struct.unpack_from("<I", data, i + 53)[0]
        blob = data[i + 57:i + 57 + size]
        i += 57 + size
        if len(blob) == size:
            out[(kind, name)][frame] = blob
    return games[game] if games else defaultdict(dict)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--before", type=int, default=1 << 30, help="only frames before this one")
    ap.add_argument("--game", type=int, default=-1, help="which game of the file (0 = first, -1 = last)")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()
    A, B = load(args.a, args.game), load(args.b, args.game)
    found = []
    for key in sorted(set(A) & set(B)):
        frames = sorted(f for f in set(A[key]) & set(B[key]) if f < args.before)
        if not frames:
            continue
        first = frames[0]
        a0, b0 = A[key][first], B[key][first]
        n = min(len(a0), len(b0)) // 4
        same_at_start = [a0[w * 4:w * 4 + 4] == b0[w * 4:w * 4 + 4] for w in range(n)]
        for w in range(n):
            if not same_at_start[w]:
                continue
            for f in frames:
                a, b = A[key][f][w * 4:w * 4 + 4], B[key][f][w * 4:w * 4 + 4]
                if a != b:
                    found.append((f, KINDS[key[0]], key[1], w * 4, a.hex(), b.hex()))
                    break
    found.sort()
    print(f"{len(found)} words split; earliest:")
    for f, kind, name, off, a, b in found[:args.top]:
        print(f"  frame {f:6} {kind:10} {name:32} +{off:<5} A {a} B {b}")


if __name__ == "__main__":
    main()
