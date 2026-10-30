"""UE3 (AO2, v445) cooked package tables + UnrealScript disassembler.
python upkdis.py <Class.Function | Class.State.Function | substring> ..."""
import os, pickle, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(os.environ.get('TEMP', HERE), 'ao2_upk_cache')
os.makedirs(CACHE, exist_ok=True)
sys.path.insert(0, HERE)
import upk

COOKED = r'C:\aot2\aot\assets\AO2Game\CookedXenon'
PKGS = ['Core', 'Engine', 'GameFramework', 'UTGame', 'IpDrv', 'AO2Game']


class Pkg:
    def __init__(self, name):
        cache = os.path.join(CACHE, name + '.pkl')
        if os.path.exists(cache):
            self.__dict__.update(pickle.load(open(cache, 'rb')))
            return
        d, (nc, no, ec, eo, ic, io) = upk.read(os.path.join(COOKED, name + '.xxx'))
        self.name = name
        self.d = d
        o = no
        self.names = []
        for _ in range(nc):
            l = struct.unpack_from('>i', d, o)[0]; o += 4
            self.names.append(d[o:o + l - 1].decode('latin1')); o += l + 8
        self.imps = []
        o = io
        for _ in range(ic):
            cp, cpn, cn, cnn, outer, on, onn = struct.unpack_from('>7i', d, o); o += 28
            self.imps.append((self.names[cn], outer, self.names[on]))
        self.exps = []
        o = eo
        for _ in range(ec):
            cls, sup, outer, nm, num, arch = struct.unpack_from('>6i', d, o); o += 24
            o += 8
            size, off, ncomp = struct.unpack_from('>3i', d, o); o += 12 + 12 * ncomp
            ef, nnet = struct.unpack_from('>2i', d, o); o += 8 + 4 * nnet + 16
            self.exps.append((cls, sup, outer, self.names[nm], size, off))
        pickle.dump(self.__dict__, open(cache, 'wb'))

    def oname(self, i):
        if i > 0:
            return self.exps[i - 1][3]
        if i < 0:
            return self.imps[-i - 1][2]
        return 'None'

    def outer(self, i):
        return self.exps[i - 1][2] if i > 0 else self.imps[-i - 1][1]

    def path(self, i):
        p = []
        while i:
            p.append(self.oname(i)); i = self.outer(i)
        return '.'.join(reversed(p))

    def cname(self, i):
        if i > 0:
            c = self.exps[i - 1][0]
            return 'Class' if c == 0 else self.oname(c)
        return self.imps[-i - 1][0]


pk = {n: Pkg(n) for n in PKGS}

# native index -> function name
natives = {}
for p in pk.values():
    for i, e in enumerate(p.exps):
        if p.cname(i + 1) != 'Function':
            continue
        b = p.d[e[5]:e[5] + e[4]]
        if len(b) < 44:
            continue
        ss = struct.unpack_from('>i', b, 40)[0]
        # the script's on-disk size is not stored; iNative sits right after it,
        # found as the tail: FriendlyName(8) [+RepOffset 2] + flags 4 + prec 1 + iNative 2
        for tail in (15, 17):
            if len(b) >= tail:
                inat = struct.unpack_from('>H', b, len(b) - tail)[0]
                flags = struct.unpack_from('>I', b, len(b) - tail + 3)[0]
                if (tail == 17) == bool(flags & 0x40):
                    if inat and flags & 0x400:
                        natives[inat] = e[3]
                    break


class Dis:
    def __init__(self, p, b):
        self.p, self.b, self.i = p, b, 0
        self.out = []

    def u8(self):
        v = self.b[self.i]; self.i += 1; return v

    def u16(self):
        v = struct.unpack_from('>H', self.b, self.i)[0]; self.i += 2; return v

    def i32(self):
        v = struct.unpack_from('>i', self.b, self.i)[0]; self.i += 4; return v

    def f32(self):
        v = struct.unpack_from('>f', self.b, self.i)[0]; self.i += 4; return v

    def obj(self):
        return self.p.oname(self.i32())

    def name(self):
        v = self.i32(); self.i32()
        return self.p.names[v] if 0 <= v < len(self.p.names) else '?%d' % v

    def parms(self):
        a = []
        while self.b[self.i] != 0x16:
            a.append(self.expr())
        self.i += 1
        return a

    def expr(self):
        t = self.u8()
        if t in (0x00, 0x01, 0x02, 0x48):
            o = self.obj()
            return {0: o, 1: 'self.' + o, 2: 'default.' + o, 0x48: o}[t]
        if t == 0x04:
            return 'return ' + self.expr()
        if t == 0x05:
            self.obj(); self.u8()
            return 'switch(%s)' % self.expr()
        if t == 0x06:
            return 'goto %04x' % self.u16()
        if t == 0x07:
            j = self.u16()
            return 'if !(%s) goto %04x' % (self.expr(), j)
        if t == 0x08:
            return 'stop'
        if t == 0x09:
            self.u16(); self.u8()
            return 'assert(%s)' % self.expr()
        if t == 0x0A:
            j = self.u16()
            return 'default:' if j == 0xFFFF else 'case %s: (next %04x)' % (self.expr(), j)
        if t == 0x0B:
            return 'nothing'
        if t == 0x0C:
            labs = []
            while True:
                n = self.name(); o = self.i32()
                if n == 'None':
                    break
                labs.append('%s@%04x' % (n, o))
            return 'labels ' + ' '.join(labs)
        if t == 0x0D:
            return 'gotolabel ' + self.expr()
        if t == 0x0E:
            self.obj()
            return self.expr()
        if t in (0x0F, 0x14, 0x44):
            a = self.expr()
            return '%s = %s' % (a, self.expr())
        if t in (0x10, 0x1A):
            i = self.expr()
            return '%s[%s]' % (self.expr(), i)
        if t == 0x11:
            a = [self.expr() for _ in range(5)]
            return 'new(%s)' % ', '.join(a)
        if t in (0x12, 0x19):
            a = self.expr(); self.u16(); self.u8()
            return '%s.%s' % (a, self.expr())
        if t == 0x13:
            c = self.obj()
            return 'class<%s>(%s)' % (c, self.expr())
        if t == 0x15:
            return '<endparm>'
        if t == 0x16:
            return ')'
        if t == 0x17:
            return 'self'
        if t == 0x18:
            self.u16()
            return self.expr()
        if t == 0x1B:
            self.u8()
            n = self.name()
            return '%s(%s)' % (n, ', '.join(self.parms()))
        if t == 0x1C:
            o = self.i32()
            return '%s(%s)' % (self.p.path(o).split('.', 1)[-1] if o else '?', ', '.join(self.parms()))
        if t == 0x1D:
            return str(self.i32())
        if t == 0x1E:
            return '%g' % self.f32()
        if t == 0x1F:
            s = self.i
            while self.b[self.i]:
                self.i += 1
            self.i += 1
            return repr(self.b[s:self.i - 1].decode('latin1'))
        if t == 0x20:
            return 'obj:' + self.p.path(self.i32())
        if t == 0x21:
            return "'%s'" % self.name()
        if t == 0x22:
            return 'rot(%d,%d,%d)' % (self.i32(), self.i32(), self.i32())
        if t == 0x23:
            return 'vect(%g,%g,%g)' % (self.f32(), self.f32(), self.f32())
        if t == 0x24:
            return 'byte %d' % self.u8()
        if t == 0x25:
            return '0'
        if t == 0x26:
            return '1'
        if t == 0x27:
            return 'true'
        if t == 0x28:
            return 'false'
        if t == 0x29:
            return 'nativeparm ' + self.obj()
        if t == 0x2A:
            return 'none'
        if t == 0x2C:
            return str(self.u8())
        if t == 0x2D:
            return self.expr()
        if t == 0x2E:
            c = self.obj()
            return '%s(%s)' % (c, self.expr())
        if t == 0x2F:
            a = self.expr(); j = self.u16()
            return 'foreach %s (end %04x)' % (a, j)
        if t == 0x30:
            return 'iteratorpop'
        if t == 0x31:
            return 'iteratornext'
        if t in (0x32, 0x33):
            self.obj()
            a = self.expr()
            return '%s %s %s' % (a, '==' if t == 0x32 else '!=', self.expr())
        if t == 0x34:
            s = self.i
            while self.b[self.i] or self.b[self.i + 1]:
                self.i += 2
            self.i += 2
            return repr(self.b[s:self.i - 2].decode('utf-16-be'))
        if t == 0x35:
            m = self.obj(); self.obj(); self.u8(); self.u8()
            return '%s.%s' % (self.expr(), m)
        if t == 0x36:
            return '%s.Length' % self.expr()
        if t == 0x37:
            n = self.name()
            return 'global.%s(%s)' % (n, ', '.join(self.parms()))
        if t == 0x38:
            c = self.u8()
            return 'cast%02x(%s)' % (c, self.expr())
        if t == 0x39:
            a = self.expr()
            return '%s.Insert(%s, %s)' % (a, self.expr(), self.expr())
        if t == 0x3A:
            self.obj()
            return 'return'
        if t in (0x3B, 0x3C, 0x3D, 0x3E):
            a = self.expr(); b = self.expr(); self.u8()
            return 'delegatecmp(%s, %s)' % (a, b)
        if t == 0x40:
            a = self.expr()
            return '%s.Remove(%s, %s)' % (a, self.expr(), self.expr())
        if t == 0x41:
            self.i32(); self.i32(); self.i32(); self.u8()
            return 'debuginfo'
        if t == 0x42:
            self.u8(); self.obj(); n = self.name()
            return 'delegate %s(%s)' % (n, ', '.join(self.parms()))
        if t == 0x43:
            n = self.name(); self.obj()
            return 'delegateprop ' + n
        if t == 0x45:
            c = self.expr(); self.u16(); a = self.expr(); self.u16()
            return '(%s ? %s : %s)' % (c, a, self.expr())
        if t == 0x46:
            a = self.expr(); self.u16()
            return '%s.Find(%s)' % (a, self.parms_one())
        if t == 0x47:
            a = self.expr(); self.u16()
            n = self.expr()
            return '%s.Find(%s, %s)' % (a, n, self.parms_one())
        if t == 0x49:
            self.u16(); e = self.expr(); self.u8()
            return 'defaultparm(%s)' % e
        if t in (0x4A, 0x50):
            return '<empty>'
        if t == 0x4B:
            return 'instdelegate ' + self.name()
        if t == 0x51:
            return self.expr()
        if t == 0x52:
            c = self.obj()
            return '%s(%s)' % (c, self.expr())
        if t == 0x53:
            return 'END'
        if t in (0x54, 0x55, 0x56):
            a = self.expr(); self.u16()
            n = {0x54: 'Add', 0x55: 'AddItem', 0x56: 'RemoveItem'}[t]
            return '%s.%s(%s)' % (a, n, self.parms_one())
        if t == 0x57:
            a = self.expr(); self.u16()
            i = self.expr()
            return '%s.InsertItem(%s, %s)' % (a, i, self.parms_one())
        if t == 0x58:
            a = self.expr(); b = self.expr(); self.u8(); c = self.expr(); self.u16()
            return 'foreach %s(%s, %s)' % (a, b, c)
        if t >= 0x60:
            if t < 0x70:
                idx = ((t - 0x60) << 8) | self.u8()
            else:
                idx = t
            return '%s(%s)' % (natives.get(idx, 'native%d' % idx), ', '.join(self.parms()))
        raise ValueError('token %02x at %x' % (t, self.i - 1))

    def parms_one(self):
        e = self.expr()
        if self.b[self.i] == 0x16:
            self.i += 1
        return e


def disasm(p, i):
    e = p.exps[i - 1]
    b = p.d[e[5]:e[5] + e[4]]
    ss = struct.unpack_from('>i', b, 40)[0]
    d = Dis(p, b[44:])
    lines = []
    while d.i < len(d.b) - 15:
        at = d.i
        try:
            s = d.expr()
        except Exception as ex:  # noqa
            lines.append('%04x  !! %s' % (at, ex)); break
        lines.append('%04x  %s' % (at, s))
        if s == 'END':
            break
    return lines


def find(q):
    res = []
    for p in pk.values():
        for i, e in enumerate(p.exps):
            if p.cname(i + 1) not in ('Function',):
                continue
            pa = p.path(i + 1)
            if pa == q or pa.endswith('.' + q):
                res.append((p, i + 1, pa))
    return res


if __name__ == '__main__':
    for q in sys.argv[1:]:
        for p, i, pa in find(q):
            print('== %s.%s' % (p.name, pa))
            print('\n'.join(disasm(p, i)))
