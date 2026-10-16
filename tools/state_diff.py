"""Compare the state_hash.log of two co-op machines (ao2_coop_state_hash).

    python tools/state_diff.py <A state_hash.log> <B state_hash.log> [--game N] [--top N] [--ignore REGEX ...]
                               [--only REGEX] [--at FRAME] [--objects]

Both logs hold, every N lockstep frames, the properties of the level's objects that changed (see
aot/src/coop_state_hash.cpp).  This replays both, sample by sample, and prints

- the properties that differ, by property name, in the order they first differed: first frame, how many objects,
  one example with both values.  What differs from the first sample on and never spreads is this machine's own
  state (camera, rendering) - add it to tools/state_ignore.txt (one regex per line, matched against
  "Class.Property"; --ignore adds more);
- the objects that exist on one machine only, by class;
- with --at FRAME: every difference that appeared at that frame, in full.

--only REGEX keeps only matching "Class.Property"; --game N compares the Nth game of the logs (default 1; a log
has one game per level start).
"""
import argparse
import os
import re
import struct
import sys
from collections import defaultdict


class Reader:
    """A log, one sample at a time: .state[key][leaf name] = value text, for the objects alive now."""

    def __init__(self, path):
        self.f = open(path, 'r', errors='replace')
        self.game = 0
        self.classes = {}    # class id -> (name, {leaf index: (type, name)})
        self.objects = {}    # object id -> key
        self.cls_of = {}     # key -> class name
        self.types = {}      # (class name, leaf name) -> type
        self.state = {}      # key -> {leaf name: value}
        self.cid = {}        # key -> class id

    def sample(self):
        """Apply the next sample; (game, frame, touched {(key, leaf)}, born [key], gone [key]) or None at the end."""
        touched, born, gone = set(), [], []
        frame = None
        for line in self.f:
            if not line.endswith('\n'):
                return None  # a line cut off (the game was killed)
            c = line[0]
            if c.isdigit():
                parts = line.split()
                key = self.objects.get(parts[0])
                if key is None:
                    continue
                leaves = self.classes[self.cid[key]][1]
                st = self.state[key]
                for item in parts[1:]:
                    idx, _, value = item.partition('=')
                    name = leaves[idx][1]
                    st[name] = value
                    touched.add((key, name))
            elif c == 'N':
                _, oid, cid, path = line.split()
                key, n = path, 1
                while key in self.state:  # two objects of one path (rare): by order of appearance
                    n += 1
                    key = '%s#%d' % (path, n)
                self.objects[oid] = key
                self.cid[key] = cid
                self.cls_of[key] = self.classes[cid][0]
                self.state[key] = {}
                born.append(key)
            elif c == 'X':
                key = self.objects.pop(line.split()[1], None)
                if key is not None:
                    del self.state[key]
                    gone.append(key)
            elif c == 'T':
                _, cid, idx, typ, name = line.split()
                self.classes[cid][1][idx] = (typ, name)
                self.types[(self.classes[cid][0], name)] = typ
            elif c == 'C':
                _, cid, name = line.split()
                self.classes[cid] = (name, {})
            elif c == 'F':
                frame = int(line.split()[1])
            elif c == 'E':
                return self.game, frame, touched, born, gone
            elif c == 'G':
                self.game += 1
                self.objects, self.state, self.cid = {}, {}, {}
                # Whatever lived before the game is gone without notice.
                touched, born, gone = set(), [], []
        return None


def decode(typ, value):
    """A plain value's text ('fff' + hex) as numbers."""
    if not typ or any(ch not in 'fib' for ch in typ) or len(value) != 2 * sum(1 if ch == 'b' else 4 for ch in typ):
        return value
    out, pos = [], 0
    raw = bytes.fromhex(value)
    for ch in typ:
        if ch == 'b':
            out.append(str(raw[pos]))
            pos += 1
        else:
            out.append(('%.9g' if ch == 'f' else '%d') % struct.unpack('>f' if ch == 'f' else '>i', raw[pos:pos + 4]))
            pos += 4
    text = out[0] if len(out) == 1 else '(' + ', '.join(out) + ')'
    return '%s [%s]' % (text, value) if 'f' in typ else text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--game', type=int, default=1)
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--ignore', action='append', default=[])
    ap.add_argument('--only')
    ap.add_argument('--at', type=int)
    ap.add_argument('--objects', action='store_true', help='list every object that is on one machine only')
    ap.add_argument('--no-ignore-file', action='store_true')
    args = ap.parse_args()

    patterns = list(args.ignore)
    ignore_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'state_ignore.txt')
    if not args.no_ignore_file and os.path.exists(ignore_file):
        for line in open(ignore_file):
            line = line.split('#')[0].strip()
            if line:
                patterns.append(line)
    ignore = re.compile('|'.join('(?:%s)' % p for p in patterns)) if patterns else None
    only = re.compile(args.only) if args.only else None

    A, B = Reader(args.a), Reader(args.b)

    def advance(r, sample):
        # The next sample of the wanted game; `sample` if it already is one.
        while sample is not None and sample[0] < args.game:
            sample = r.sample()
        return sample if sample is not None and sample[0] == args.game else None

    sa, sb = advance(A, A.sample()), advance(B, B.sample())
    differing = {}                  # (key, leaf) -> first frame (differs now)
    first = {}                      # leaf name -> [first frame, example (key, va, vb), set of keys, samples]
    ever = defaultdict(set)         # leaf name -> keys that ever differed
    lonely = {}                     # key -> (side, frame) : on one machine only now
    lonely_first = {}               # class -> [first frame, side counts, example]
    at_frame = []
    samples = last = 0
    first_frame = None

    def wanted(cls, leaf):
        name = cls + '.' + leaf
        return not (ignore and ignore.search(name)) and (not only or only.search(name))

    while sa is not None and sb is not None:
        if sa[1] != sb[1]:  # a sample one log lacks
            if sa[1] < sb[1]:
                sa = advance(A, A.sample())
            else:
                sb = advance(B, B.sample())
            continue
        frame = sa[1]
        samples += 1
        last = frame
        # Objects on one machine only.
        for key in set(sa[3]) | set(sb[3]) | set(sa[4]) | set(sb[4]):
            in_a, in_b = key in A.state, key in B.state
            if in_a == in_b:
                lonely.pop(key, None)
                continue
            if key not in lonely:
                side = 'A' if in_a else 'B'
                lonely[key] = (side, frame)
                cls = (A if in_a else B).cls_of.get(key, '?')
                entry = lonely_first.setdefault(cls, [frame, {'A': 0, 'B': 0}, key])
                entry[1][side] += 1
                if args.objects:
                    print('only %s  F %d  %s  (%s)' % (side, frame, key, cls))
        # Properties.
        touched = sa[2] | sb[2]
        for key in set(sa[3]) | set(sb[3]):  # an object new on one side: all it wrote on the other
            for side in (A, B):
                touched.update((key, leaf) for leaf in side.state.get(key, ()))
        for key, leaf in touched:
            if key not in A.state or key not in B.state:
                differing.pop((key, leaf), None)
                continue
            va, vb = A.state[key].get(leaf), B.state[key].get(leaf)
            if va == vb:
                differing.pop((key, leaf), None)
                continue
            cls = A.cls_of.get(key, '?')
            if not wanted(cls, leaf):
                continue
            if (key, leaf) not in differing:
                differing[(key, leaf)] = frame
                if first_frame is None:
                    first_frame = frame
                typ = A.types.get((cls, leaf), '')
                if leaf not in first:
                    first[leaf] = [frame, (key, cls, decode(typ, va or '0'), decode(typ, vb or '0'))]
                ever[leaf].add(key)
                if args.at == frame:
                    at_frame.append((key, cls, leaf, decode(typ, va or '0'), decode(typ, vb or '0')))
        sa, sb = advance(A, A.sample()), advance(B, B.sample())

    if not samples:
        print('no common samples in game %d (A has %d game(s), B has %d)' % (args.game, A.game, B.game))
        return 1
    print('game %d: %d common samples, frames up to %d' % (args.game, samples, last))
    if first_frame is None:
        print('no property differs' + (' (ignoring %d patterns)' % len(patterns) if patterns else ''))
    else:
        print('first difference at frame %d; %d properties differ at the end' % (first_frame, len(differing)))
    now = defaultdict(int)
    for (key, leaf) in differing:
        now[leaf] += 1
    rows = sorted(first.items(), key=lambda kv: (kv[1][0], kv[0]))
    if rows:
        print()
        print('%-8s %-6s %-6s %s' % ('frame', 'ever', 'now', 'property: example object (class)  A / B'))
    for leaf, (frame, (key, cls, va, vb)) in rows[:args.top]:
        print('%-8d %-6d %-6d %s: %s (%s)\n%29s%s / %s' % (frame, len(ever[leaf]), now.get(leaf, 0), leaf, key, cls,
                                                        '', va, vb))
    if len(rows) > args.top:
        print('... %d more (--top)' % (len(rows) - args.top))
    if lonely_first:
        print()
        print('objects on one machine only (first frame, class, how many on A / B, example):')
        for cls, (frame, counts, key) in sorted(lonely_first.items(), key=lambda kv: kv[1][0])[:args.top]:
            print('%-8d %-36s %d / %d  %s' % (frame, cls, counts['A'], counts['B'], key))
    if args.at is not None:
        print()
        print('differences that appeared at frame %d:' % args.at)
        for key, cls, leaf, va, vb in sorted(at_frame):
            print('  %s (%s).%s: %s / %s' % (key, cls, leaf, va, vb))
    return 0


if __name__ == '__main__':
    sys.exit(main())
