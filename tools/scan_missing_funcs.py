"""Find guest code that is referenced but was never recompiled.

A function the analyzer did not discover (reached only through a function
pointer: a callback built with lis/addi, a vtable or a table in data) is not
emitted at all, and an indirect call to it aborts the game. This scans
  * lis/addi|ori constants in recompiled code, and
  * aligned big-endian words anywhere in the image (vtables, tables),
for addresses inside the code range that lie in a gap between recompiled
functions, and prints them as default_functions.toml entries.

Usage: python scan_missing_funcs.py [--data]   (needs fnindex.pkl, image.bin)
"""
import bisect
import os
import struct
import sys

import fnindex

BLR = 0x4E800020
BCTR = 0x4E800420


def main():
    db = fnindex.load()
    funcs = sorted((a, fn["n"]) for a, fn in db["funcs"].items() if isinstance(a, int))
    starts = [a for a, _ in funcs]
    start_set = set(starts)
    lo, hi = starts[0], funcs[-1][0] + 4 * funcs[-1][1]
    img = open(fnindex.IMAGE, "rb").read()

    def word(addr):
        o = addr - 0x82000000
        return struct.unpack_from(">I", img, o)[0] if 0 <= o + 4 <= len(img) else None

    def covered(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and addr < funcs[i][0] + 4 * funcs[i][1]

    def plausible(addr):
        w = word(addr)
        if not w:
            return False
        prev = word(addr - 4)
        # Entry right after a function end (blr / bctr / padding / tail branch) or a known function.
        return prev in (BLR, BCTR, 0) or covered(addr - 4) or (prev is not None and prev >> 26 == 18)

    def ends_function(addr):
        return word(addr - 4) in (BLR, BCTR, 0)

    def containing(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 and addr < funcs[i][0] + 4 * funcs[i][1] else None

    def in_jump_table(src, target):
        # A switch table: neighbouring words point into the same function.
        f = containing(target)
        for n in (src - 4, src + 4):
            v = word(n) if lo <= n or True else None
            if v is not None and v != target and f is not None and containing(v) == f:
                return True
        return False

    # Import thunks and the register save/restore helpers are not functions here.
    reg = os.path.join(fnindex.GEN, "armyoftworecomp_register.cpp")
    imports = set()
    if os.path.exists(reg):
        for line in open(reg, encoding="utf-8", errors="replace"):
            if "SetFunction(0x" in line and ", __imp__" in line and "__imp__sub_" not in line:
                imports.add(int(line.split("SetFunction(")[1].split(",")[0], 16))

    def excluded(c):
        return c in imports or 0x82E28DA0 <= c < 0x82E29AA0

    refs = {}
    for a, fn in db["funcs"].items():
        for c in fn["consts"]:
            # A function taking the address of its own label is a computed jump, not a callback.
            if lo <= c < hi and c % 4 == 0 and c not in start_set and containing(c) != a:
                refs.setdefault(c, set()).add(fnindex.H(a))
    if "--data" in sys.argv:
        for o in range(0, len(img) - 3, 4):
            v = struct.unpack_from(">I", img, o)[0]
            if lo <= v < hi and v % 4 == 0 and v not in start_set:
                src = 0x82000000 + o
                if not (lo <= src < hi) and not in_jump_table(src, v):  # data, not code / switch tables
                    refs.setdefault(v, set()).add(f"data@{src:08X}")
    # Uncovered code, plus code GapFill appended to the previous function
    # (covered, but right after that function's return).
    missing = sorted(c for c in refs if not excluded(c) and
                     ((not covered(c) and plausible(c)) or (covered(c) and ends_function(c))))
    for c in missing:
        srcs = sorted(refs[c])
        print(f"0x{c:08X} = {{}}  # {' '.join(srcs[:4])}{' ...' if len(srcs) > 4 else ''}")
    print(f"# {len(missing)} uncovered referenced entry points (code range {lo:08X}-{hi:08X})",
          file=sys.stderr)


if __name__ == "__main__":
    main()
