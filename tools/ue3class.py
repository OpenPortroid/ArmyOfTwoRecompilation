"""Read UE3 objects of a class with their property values (running game, guest memory, reflection).

    python tools/ue3class.py <pid> props <Class>              -> the class's properties: offset, type, owner class
    python tools/ue3class.py <pid> objs <Class> [Prop ...]    -> live instances (not Default__) with property values
                                                               (all, or only the named ones)
UProperty: offset +0x64, ElementSize +0x48, ArrayDim +0x44; UBoolProperty mask +0x80; Object/Class/Struct/Array
properties point to their class / struct / inner property at +0x80.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ue3net  # noqa: E402

OUTER, NAME, NUMBER, CLASS, SUPER = 40, 44, 48, 52, 60
ARRAYDIM, ELEM, POFF, EXTRA = 0x44, 0x48, 0x64, 0x80


class Reflect:
    def __init__(self, pid):
        self.m = ue3net.Mem(pid)
        self.names = ue3net.Names(self.m)
        self.objs = [o for o in ue3net.objects(self.m) if o]
        self.cname = {}
        self.by_outer = {}
        self.classes = {}
        for o in self.objs:
            c = self.cls(o)
            if c.endswith('Property'):
                self.by_outer.setdefault(self.m.u32(o + OUTER), []).append(o)
            elif c in ('Class', 'ScriptStruct', 'Struct'):
                self.classes.setdefault(self.oname(o), o)

    def oname(self, o):
        if not o:
            return 'None'
        n = self.names(self.m.u32(o + NAME))
        num = self.m.u32(o + NUMBER)
        return n + ('_%d' % (num - 1) if num else '')

    def cls(self, o):
        c = self.m.u32(o + CLASS)
        if c not in self.cname:
            self.cname[c] = self.names(self.m.u32(c + NAME)) if c else '?'
        return self.cname[c]

    def props(self, struct_obj):
        """[(offset, name, type, property object, owner)] of a class / struct and its supers."""
        out = []
        c = struct_obj
        while c:
            for p in self.by_outer.get(c, []):
                out.append((self.m.u32(p + POFF), self.oname(p), self.cls(p), p, self.oname(c)))
            c = self.m.u32(c + SUPER)
        return sorted(out)

    def value(self, addr, ptype, prop, depth=0):
        m = self.m
        if ptype == 'IntProperty':
            return str(struct.unpack('>i', m.read(addr, 4))[0])
        if ptype == 'FloatProperty':
            return '%g' % struct.unpack('>f', m.read(addr, 4))[0]
        if ptype == 'ByteProperty':
            return str(m.read(addr, 1)[0])
        if ptype == 'BoolProperty':
            return '1' if m.u32(addr) & m.u32(prop + EXTRA) else '0'
        if ptype == 'NameProperty':
            return self.names(m.u32(addr))
        if ptype in ('ObjectProperty', 'ClassProperty', 'ComponentProperty', 'InterfaceProperty'):
            o = m.u32(addr)
            return '%08X %s' % (o, self.oname(o)) if o else 'None'
        if ptype == 'StrProperty':
            data, n = m.u32(addr), m.u32(addr + 4)
            raw = m.read(data, min(n, 300)) if data and n else b''  # FString of char (TCHAR = char in this build)
            return repr(raw.split(b'\0')[0].decode('latin1')) if raw else "''"
        if ptype == 'StructProperty':
            st = m.u32(prop + EXTRA)
            sname = self.oname(st)
            if sname in ('Vector', 'Rotator') or depth > 2:
                if sname == 'Vector':
                    return '(%g, %g, %g)' % struct.unpack('>3f', m.read(addr, 12))
                if sname == 'Rotator':
                    return '(%d, %d, %d)' % struct.unpack('>3i', m.read(addr, 12))
                return '<%s>' % sname
            return '{' + ', '.join('%s=%s' % (n, self.value(addr + off, t, p, depth + 1))
                                   for off, n, t, p, _ in self.props(st)) + '}'
        if ptype == 'ArrayProperty':
            data, n = m.u32(addr), m.u32(addr + 4)
            inner = m.u32(prop + EXTRA)
            it, isz = self.cls(inner), m.u32(inner + ELEM)
            if not data or not n or n > 100000:
                return '[] (n=%s)' % n
            shown = [self.value(data + i * isz, it, inner, depth + 1) for i in range(min(n, 64))]
            return '[%d: ' % n + ', '.join(shown) + (' ...' if n > 64 else '') + ']'
        return '<%s %s>' % (ptype, (m.read(addr, 8) or b'').hex())


def main():
    r = Reflect(int(sys.argv[1]))
    cmd, cname = sys.argv[2], sys.argv[3]
    want = set(sys.argv[4:])
    cls = r.classes.get(cname)
    if not cls:
        raise SystemExit('no class ' + cname)
    plist = r.props(cls)
    if cmd == 'props':
        for off, n, t, p, owner in plist:
            extra = ''
            if t in ('StructProperty', 'ObjectProperty', 'ClassProperty', 'ArrayProperty', 'ComponentProperty'):
                x = r.m.u32(p + EXTRA)
                extra = ' -> ' + (r.cls(x) if t == 'ArrayProperty' and x else '') + ' ' + r.oname(x)
            print('+%-5d %-28s %-18s %s%s' % (off, n, t.replace('Property', ''), owner, extra))
        return
    for o in r.objs:
        if r.cls(o) != cname or r.oname(o).startswith('Default__'):
            continue
        print('%08X %s (outer %s)' % (o, r.oname(o), r.oname(r.m.u32(o + OUTER))))
        for off, n, t, p, owner in plist:
            if want and n not in want:
                continue
            try:
                v = r.value(o + off, t, p)
            except (TypeError, struct.error):
                v = '?'
            print('    %-28s %s' % (n, v))


if __name__ == '__main__':
    main()
