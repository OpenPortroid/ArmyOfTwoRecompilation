"""Explore UE3 reflection data in a running game (guest memory).

python ue3probe.py <pid> name <Name>        -> FNameEntry addresses holding <Name>
python ue3probe.py <pid> objects <Name>     -> objects named <Name> (GObjObjects scan) + dump
python ue3probe.py <pid> dump <addr> [size] -> hex/BE dwords of guest memory
Guest virtual memory is at host 0x100000000 + address.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
BASE = 0x100000000
GOBJ = 0x831E4E3C


class Mem:
    def __init__(self, pid):
        self.h = k32.OpenProcess(0x0410, False, pid)

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        n = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, BASE + addr, buf, size, ctypes.byref(n)):
            return None
        return buf.raw

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('>I', b)[0] if b else None


def find_name_entries(m, name):
    needle = name.encode() + b'\0'
    hits = []
    for region in range(0x40000000, 0x60000000, 0x100000):
        b = m.read(region, 0x100000)
        if not b:
            continue
        i = b.find(needle)
        while i >= 0:
            # FNameEntry: Index (int), HashNext (ptr), name chars
            if i >= 8:
                idx, nxt = struct.unpack('>iI', b[i - 8:i])
                hits.append((region + i - 8, idx, nxt))
            i = b.find(needle, i + 1)
    return hits


def objects(m):
    data, count = m.u32(GOBJ), m.u32(GOBJ + 4)
    raw = m.read(data, count * 4)
    return [struct.unpack_from('>I', raw, i * 4)[0] for i in range(count)]


def dump(m, addr, size=0x80):
    b = m.read(addr, size)
    for off in range(0, size, 16):
        words = struct.unpack_from('>4I', b, off)
        print('  +%03X: %s' % (off, ' '.join('%08X' % w for w in words)))


def main():
    m = Mem(int(sys.argv[1]))
    cmd = sys.argv[2]
    if cmd == 'name':
        for a, idx, nxt in find_name_entries(m, sys.argv[3]):
            print('entry %08X index %d (0x%X) next %08X' % (a, idx, idx, nxt))
    elif cmd == 'objects':
        entries = find_name_entries(m, sys.argv[3])
        idxs = set()
        for a, idx, nxt in entries:
            idxs.add(idx)
            idxs.add(idx >> 1)
        print('name indices', sorted(idxs))
        for o in objects(m):
            if not o:
                continue
            n = m.u32(o + 44)
            if n in idxs:
                print('object %08X name idx %d class %08X outer %08X' % (o, n, m.u32(o + 52), m.u32(o + 40)))
                dump(m, o, int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x80)
    elif cmd == 'dump':
        dump(m, int(sys.argv[3], 16), int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x80)


if __name__ == '__main__':
    main()
