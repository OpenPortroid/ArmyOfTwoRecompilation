"""Survey UE3 replication data of loaded classes in a running game (guest memory).

python ue3net.py <pid> layout <PropName>   -> dump UProperty objects named <PropName> (find field offsets)
python ue3net.py <pid> survey [--all]       -> per class: properties with CPF_Net, NetFields count
Uses ue3probe.Mem; guest virtual memory at host 0x100000000 + address.
"""
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ue3probe  # noqa: E402
Mem, find_name_entries, objects = ue3probe.Mem, ue3probe.find_name_entries, ue3probe.objects

OUTER, NAME, CLASS = 40, 44, 52


class Names:
    """FName index -> string via GNames (FName::Names TArray at 0x831D1724, chars at entry+16)."""
    GNAMES_ARRAY = 0x831D1724

    def __init__(self, m):
        self.m = m
        self.cache = {}

    def __call__(self, idx):
        if idx in self.cache:
            return self.cache[idx]
        e = self.m.u32(self.m.u32(self.GNAMES_ARRAY) + idx * 4) if idx is not None and idx < 0x100000 else None
        s = '?%s' % idx
        if e:
            b = self.m.read(e + 16, 96) or b''
            s = b.split(b'\0')[0].decode('latin1')
        self.cache[idx] = s
        return s


def main():
    m = Mem(int(sys.argv[1]))
    cmd = sys.argv[2]
    names = Names(m)
    objs = [o for o in objects(m) if o]
    cls_name = {}

    def oname(o):
        return names(m.u32(o + NAME))

    def ocls(o):
        c = m.u32(o + CLASS)
        if c not in cls_name:
            cls_name[c] = oname(c) if c else '?'
        return cls_name[c]

    if cmd == 'layout':
        want = sys.argv[3]
        for o in objs:
            if oname(o) == want:
                print('%08X %s outer=%s' % (o, ocls(o), oname(m.u32(o + OUTER))))
                ue3probe.dump(m, o, 0xA0)
    elif cmd == 'survey':
        flags_off = int(os.environ.get('FLAGS_OFF', '0x50'), 16)
        per_class = defaultdict(list)
        n_props = 0
        for o in objs:
            k = ocls(o)
            if not k.endswith('Property'):
                continue
            n_props += 1
            hi, lo = struct.unpack('>II', m.read(o + flags_off, 8))
            if hi & 0x20:  # CPF_Net (DWORD at +0x50)
                per_class[oname(m.u32(o + OUTER))].append(oname(o))
        print('properties: %d, classes with CPF_Net props: %d' % (n_props, len(per_class)))
        show_all = '--all' in sys.argv
        for c in sorted(per_class):
            if show_all or c.startswith('AO2'):
                print('%-40s %3d  %s' % (c, len(per_class[c]), ' '.join(per_class[c][:12])))


if __name__ == '__main__':
    main()
