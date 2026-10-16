"""For each D3D function, list stores/loads at device offsets that fall in the
register shadow, translated to Xenos register names.

python devstores.py [addr ...]      (default: all functions in the trace 'ever called' list)
Heuristic: device pointer = r3 at entry, tracked through `mr rX,r3`.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fnindex  # noqa: E402

REGS = {}
for m in re.finditer(r"XE_GPU_REGISTER\((0x[0-9A-Fa-f]+),\s*\w+,\s*(\w+)\)",
                     open(r"C:\aot2\sdk\include\rex\graphics\register_table.inc").read()):
    REGS[int(m.group(1), 16)] = m.group(2)

# device offset -> base register of shadow block
BLOCKS = [
    (1152, 1920, 0x4800, "FETCH"),
    (1920, 6016, 0x4000, "VSCONST"),
    (6016, 10112, 0x4400, "PSCONST"),
    (10112, 10272, 0x4900, "BOOLLOOP"),
    (10368, 10432, 0x2000, None),
    (10432, 10444, 0x2080, None),
    (10444, 10528, 0x2100, None),
    (10528, 10548, 0x2180, None),
    (10548, 10596, 0x2200, None),
    (10596, 10680, 0x2280, None),
    (10680, 10832, 0x2300, None),
    (10832, 10928, 0x2380, None),
]


def name_off(off):
    for lo, hi, reg, tag in BLOCKS:
        if lo <= off < hi:
            r = reg + (off - lo) // 4
            if tag:
                return f"{tag}[{(off - lo) // 4}]"
            return REGS.get(r, f"reg{r:04X}")
    return None


def analyze(addr, db):
    fn = db["funcs"][addr]
    dev = {"r3"}
    out = []
    path = os.path.join(fnindex.GEN, fn["file"])
    with open(path, encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f, 1):
            if i <= fn["line"]:
                continue
            if line.startswith("DEFINE_REX_FUNC") or line.startswith("}"):
                break
            m = re.match(r"\t// (\S+)\s*(.*)", line)
            if not m:
                continue
            mn, ops = m.group(1), [x.strip() for x in m.group(2).split(",")]
            if mn == "mr" and len(ops) == 2:
                if ops[1] in dev:
                    dev.add(ops[0])
                else:
                    dev.discard(ops[0])
                continue
            mm = re.match(r"^(-?\d+)\((r\d+)\)$", ops[-1]) if ops else None
            if mm and mm.group(2) in dev and (mn.startswith("st") or mn.startswith("l")):
                off = int(mm.group(1))
                nm = name_off(off)
                out.append((("W" if mn.startswith("st") else "R"), off, nm))
            elif ops and ops[0] in dev and mn not in ("cmplwi", "cmpwi", "cmplw", "cmpw", "stw", "std"):
                if not (mn.startswith("st") or mn.startswith("cmp")):
                    dev.discard(ops[0])
    return out


def main():
    db = fnindex.load()
    if len(sys.argv) > 1:
        addrs = [int(a, 16) for a in sys.argv[1:]]
    else:
        tr = open(r"C:\aot2\aot\out\build\win-amd64-release\d3d_trace.txt").read()
        addrs = [int(x, 16) for x in re.findall(r"[0-9A-F]{8}", tr[tr.rfind("--- ever"):])]
    for a in addrs:
        acc = analyze(a, db)
        ws = sorted({(o, n) for k, o, n in acc if k == "W"})
        rs = sorted({(o, n) for k, o, n in acc if k == "R"})
        print(f"{a:08X} n={db['funcs'][a]['n']}")
        print("   W:", " ".join(f"{o}{'=' + n if n else ''}" for o, n in ws)[:600])
        print("   R:", " ".join(f"{o}{'=' + n if n else ''}" for o, n in rs)[:600])


if __name__ == "__main__":
    main()
