"""Survey live UE3 objects to find what identifies the current menu screen.

python tools/menuprobe.py <pid> classes [substr]   -> count of objects per class name (filter by substring)
python tools/menuprobe.py <pid> snap <file>        -> save {object addr: class/name} to a file
python tools/menuprobe.py <pid> diff <file>        -> objects added/removed since the snap
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ue3net  # noqa: E402

OUTER, NAME, NUMBER, CLASS = 40, 44, 48, 52


def listing(m, names):
    out = {}
    for o in ue3net.objects(m):
        if not o:
            continue
        hdr = m.read(o, 64)
        if not hdr:
            continue
        import struct
        outer, nm, num, cls = struct.unpack_from('>IIII', hdr, 40)[0:4] if False else (
            struct.unpack_from('>I', hdr, 40)[0], struct.unpack_from('>I', hdr, 44)[0],
            struct.unpack_from('>i', hdr, 48)[0], struct.unpack_from('>I', hdr, 52)[0])
        cname = names(m.u32(cls + NAME)) if cls else '?'
        n = names(nm) + ('_%d' % (num - 1) if num > 0 else '')
        out[o] = '%s %s' % (cname, n)
    return out


def main():
    m = ue3net.Mem(int(sys.argv[1]))
    names = ue3net.Names(m)
    cmd = sys.argv[2]
    lst = listing(m, names)
    if cmd == 'classes':
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        cnt = {}
        for v in lst.values():
            c = v.split(' ', 1)[0]
            cnt[c] = cnt.get(c, 0) + 1
        for c, n in sorted(cnt.items(), key=lambda x: -x[1]):
            if flt in c.lower():
                print(n, c)
    elif cmd == 'snap':
        with open(sys.argv[3], 'w') as f:
            for a, v in lst.items():
                f.write('%08X %s\n' % (a, v))
        print(len(lst), 'objects')
    elif cmd == 'diff':
        old = {}
        for line in open(sys.argv[3]):
            a, v = line.rstrip('\n').split(' ', 1)
            old[int(a, 16)] = v
        for a, v in lst.items():
            if old.get(a) != v:
                print('+ %08X %s' % (a, v))
        for a, v in old.items():
            if lst.get(a) != v and a not in lst:
                print('- %08X %s' % (a, v))


main()
