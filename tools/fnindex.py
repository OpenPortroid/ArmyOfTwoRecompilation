"""Index recompiled guest functions from generated C++ (asm comments).

Build:  python fnindex.py build            -> tools/fnindex.pkl
Query:  python fnindex.py info  0x82A567F8 [--asm]
        python fnindex.py callers 0x82A567F8
        python fnindex.py tree  0x82A567F8 [depth]
        python fnindex.py const 0xC0002200 [mask]   (functions materializing a constant)
        python fnindex.py global 0x83000000 [0x83010000]  (functions touching a global range)
        python fnindex.py range 0x82A50000 0x82A70000     (list functions in range)
        python fnindex.py imports VdSwap                   (functions calling an import)
"""
import glob
import os
import pickle
import re
import sys

GEN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "aot", "generated", "default")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fnindex.pkl")

RE_FUNC = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
RE_ASM = re.compile(r"^\t// (\S+)\s*(.*)$")
RE_IMP = re.compile(r"__imp__(\w+)\(ctx")
RE_NUM = re.compile(r"^-?(0x[0-9a-fA-F]+|\d+)$")


def num(s):
    s = s.strip()
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    v = int(s, 16) if s.startswith("0x") else int(s)
    return -v if neg else v


def u32(v):
    return v & 0xFFFFFFFF


def parse_mem(op):
    # "16560(r31)" -> (16560, 'r31')
    m = re.match(r"^(-?\d+)\((r\d+)\)$", op)
    if m:
        return int(m.group(1)), m.group(2)
    return None


def build():
    funcs = {}
    files = sorted(glob.glob(os.path.join(GEN, "armyoftworecomp_recomp.*.cpp")))
    for fi, path in enumerate(files):
        cur = None
        regs = {}
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for ln, line in enumerate(f, 1):
                if line.startswith("DEFINE_REX_FUNC("):
                    m = RE_FUNC.match(line)
                    name = m.group(1)
                    addr = int(name[4:], 16) if name.startswith("sub_") else None
                    cur = {
                        "name": name, "addr": addr, "file": os.path.basename(path), "line": ln,
                        "n": 0, "calls": set(), "imports": set(), "consts": set(),
                        "globals": set(), "branches": set(), "indirect": 0,
                    }
                    funcs[addr if addr is not None else name] = cur
                    regs = {}
                    continue
                if cur is None:
                    continue
                if line.startswith("loc_"):
                    regs = {}  # join point: forget constant tracking
                    continue
                if not line.startswith("\t// "):
                    if "__imp__" in line:
                        m = RE_IMP.search(line)
                        if m and not m.group(1).startswith("sub_"):
                            cur["imports"].add(m.group(1))
                    continue
                m = RE_ASM.match(line.rstrip("\n"))
                if not m:
                    continue
                mn, ops = m.group(1), m.group(2)
                cur["n"] += 1
                o = [x.strip() for x in ops.split(",")] if ops else []
                try:
                    if mn == "bl" and o:
                        cur["calls"].add(int(o[0], 16))
                    elif mn in ("b",) and o and o[0].startswith("0x"):
                        cur["branches"].add(int(o[0], 16))
                    elif mn in ("bctrl", "bctr"):
                        cur["indirect"] += 1
                    elif mn == "li" and len(o) == 2:
                        regs[o[0]] = u32(num(o[1]))
                    elif mn == "lis" and len(o) == 2:
                        regs[o[0]] = u32(num(o[1]) << 16)
                    elif mn in ("ori", "oris", "addi", "addis") and len(o) == 3:
                        src = regs.get(o[1])
                        if src is not None and o[1] != "r0":
                            imm = num(o[2])
                            if mn == "ori":
                                v = src | (imm & 0xFFFF)
                            elif mn == "oris":
                                v = src | ((imm & 0xFFFF) << 16)
                            elif mn == "addi":
                                v = u32(src + imm)
                            else:
                                v = u32(src + (imm << 16))
                            regs[o[0]] = v
                            cur["consts"].add(v)
                            if 0x82000000 <= v < 0x84000000 or 0x40000000 <= v < 0x80000000:
                                cur["globals"].add(v)
                        else:
                            regs.pop(o[0], None)
                    elif mn[0] == "l" and len(o) == 2 and "(" in o[1]:
                        pm = parse_mem(o[1])
                        if pm and pm[1] in regs:
                            ga = u32(regs[pm[1]] + pm[0])
                            if 0x82000000 <= ga < 0x84000000:
                                cur["globals"].add(ga)
                        regs.pop(o[0], None)
                    elif mn[0:2] == "st" and len(o) == 2 and "(" in o[1]:
                        pm = parse_mem(o[1])
                        if pm and pm[1] in regs:
                            ga = u32(regs[pm[1]] + pm[0])
                            if 0x82000000 <= ga < 0x84000000:
                                cur["globals"].add(ga)
                        if mn.endswith("u") and pm:
                            regs.pop(pm[1], None)
                    elif o:
                        regs.pop(o[0], None)
                except ValueError:
                    if o:
                        regs.pop(o[0], None)
        print(f"[{fi + 1}/{len(files)}] {os.path.basename(path)} funcs={len(funcs)}", file=sys.stderr)
    # callers
    callers = {}
    for k, fn in funcs.items():
        for c in fn["calls"] | (fn["branches"] & set(x for x in funcs if isinstance(x, int))):
            callers.setdefault(c, set()).add(k)
    with open(OUT, "wb") as f:
        pickle.dump({"funcs": funcs, "callers": callers}, f, protocol=pickle.HIGHEST_PROTOCOL)
    print(f"wrote {OUT}: {len(funcs)} functions")


def load():
    with open(OUT, "rb") as f:
        return pickle.load(f)


IMAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "image.bin")  # memdump of 0x82000000
_img = None


def image_str(addr):
    """Printable C string at a guest address in the dumped image, or None."""
    global _img
    if _img is None:
        _img = open(IMAGE, "rb").read() if os.path.exists(IMAGE) else b""
    o = addr - 0x82000000
    if not (0 <= o < len(_img)):
        return None
    e = _img.find(b"\0", o, o + 256)
    if e - o < 4:
        return None
    s = _img[o:e]
    if all(32 <= c < 127 or c in (9, 10) for c in s):
        return s.decode()
    return None


def H(x):
    return f"0x{x:08X}" if isinstance(x, int) else str(x)


def show(fn, db, asm=False):
    print(f"{fn['name']}  {fn['file']}:{fn['line']}  instrs={fn['n']} indirect={fn['indirect']}")
    print("  calls:  ", " ".join(H(c) for c in sorted(fn["calls"])))
    tails = sorted(b for b in fn["branches"] if b in db["funcs"] and b != fn["addr"])
    if tails:
        print("  tails:  ", " ".join(H(c) for c in tails))
    if fn["imports"]:
        print("  imports:", " ".join(sorted(fn["imports"])))
    print("  callers:", " ".join(H(c) for c in sorted(db["callers"].get(fn["addr"], ()), key=str)))
    g = sorted(fn["globals"])
    if g:
        print("  globals:", " ".join(H(c) for c in g[:40]), "..." if len(g) > 40 else "")
        for a in g:
            s = image_str(a)
            if s:
                print(f"    {H(a)}: {s!r}")
    big = sorted(c for c in fn["consts"] if c > 0xFFFF and c not in fn["globals"])
    if big:
        print("  consts: ", " ".join(H(c) for c in big[:40]))
    if asm:
        path = os.path.join(GEN, fn["file"])
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f, 1):
                if i < fn["line"]:
                    continue
                if i > fn["line"] and (line.startswith("DEFINE_REX_FUNC") or line.startswith("}")):
                    break
                if line.startswith("\t// ") or line.startswith("loc_"):
                    print("   ", line.rstrip()[1:] if line.startswith("\t") else line.rstrip())


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "build":
        build()
        return
    db = load()
    funcs = db["funcs"]
    a = sys.argv[2:]
    if cmd == "info":
        fn = funcs.get(int(a[0], 16))
        if not fn:
            # find containing function
            addr = int(a[0], 16)
            cands = [k for k in funcs if isinstance(k, int) and k <= addr]
            fn = funcs[max(cands)]
            print(f"(containing function of {H(addr)})")
        show(fn, db, "--asm" in a)
    elif cmd == "callers":
        for c in sorted(db["callers"].get(int(a[0], 16), ()), key=str):
            f = funcs.get(c)
            print(H(c), f["n"] if f else "")
    elif cmd == "tree":
        root = int(a[0], 16)
        depth = int(a[1]) if len(a) > 1 else 2
        seen = set()

        def rec(x, d):
            f = funcs.get(x)
            print("  " * d + H(x) + (f" n={f['n']} {' '.join(sorted(f['imports']))}" if f else " ?"))
            if not f or d >= depth or x in seen:
                return
            seen.add(x)
            for c in sorted(f["calls"]):
                rec(c, d + 1)
        rec(root, 0)
    elif cmd == "const":
        v = int(a[0], 16)
        mask = int(a[1], 16) if len(a) > 1 else 0xFFFFFFFF
        for k, f in sorted(funcs.items(), key=lambda kv: str(kv[0]) if not isinstance(kv[0], int) else f"{kv[0]:08X}"):
            hits = sorted(c for c in f["consts"] if (c & mask) == (v & mask))
            if hits:
                print(H(k), f["n"], " ".join(H(h) for h in hits[:8]))
    elif cmd == "global":
        lo = int(a[0], 16)
        hi = int(a[1], 16) if len(a) > 1 else lo + 4
        for k, f in sorted(funcs.items(), key=lambda kv: str(kv[0])):
            hits = sorted(g for g in f["globals"] if lo <= g < hi)
            if hits:
                print(H(k), f["n"], " ".join(H(h) for h in hits[:8]))
    elif cmd == "range":
        lo, hi = int(a[0], 16), int(a[1], 16)
        for k in sorted(x for x in funcs if isinstance(x, int) and lo <= x < hi):
            f = funcs[k]
            nc = len(db["callers"].get(k, ()))
            print(f"{H(k)} n={f['n']:5d} callers={nc:4d} calls={len(f['calls']):3d} {' '.join(sorted(f['imports']))}")
    elif cmd == "imports":
        for k, f in funcs.items():
            if any(a[0] in i for i in f["imports"]):
                print(H(k), " ".join(sorted(f["imports"])))


if __name__ == "__main__":
    main()
