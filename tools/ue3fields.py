"""Name the fields at given offsets of an object's class (running game, UE3 reflection).

    python tools/ue3fields.py <pid> <object name, e.g. AO2Char_irq05_iraqi_elite_0> <kind pawn|controller|mesh> <offset> ...
    python tools/ue3fields.py <pid> class:<UClass address hex> - <offset> ...

Finds the object (pawn by name; its Controller / Mesh through the pawn's
properties), walks its class chain (UStruct +60 SuperField) and prints the
properties (Outer = a class of the chain, UProperty +0x64 offset) that cover
each offset.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ue3net  # noqa: E402  (Mem, Names, objects)

OUTER, NAME, NUMBER, CLASS, SUPER, POFF, ELEM = 40, 44, 48, 52, 60, 0x64, 0x48


def main():
    pid, want, kind = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    offsets = [int(x, 0) for x in sys.argv[4:]]
    m = ue3net.Mem(pid)
    names = ue3net.Names(m)
    objs = [o for o in ue3net.objects(m) if o]

    def oname(o):
        n = names(m.u32(o + NAME))
        num = m.u32(o + NUMBER)
        return n + (f"_{num - 1}" if num else "")

    by_outer = {}
    for o in objs:
        c = m.u32(o + CLASS)
        cn = names(m.u32(c + NAME)) if c else ""
        if cn.endswith("Property"):
            by_outer.setdefault(m.u32(o + OUTER), []).append(o)

    def props(cls):
        out = []
        c = cls
        while c:
            for p in by_outer.get(c, []):
                out.append((m.u32(p + POFF), names(m.u32(p + NAME)), names(m.u32(m.u32(p + CLASS) + NAME)),
                            names(m.u32(c + NAME))))
            c = m.u32(c + SUPER)
        return sorted(out)

    if want.startswith("class:"):
        cls = int(want[6:], 16)
        print(f"class {names(m.u32(cls + NAME))}")
        plist = props(cls)
        for want_off in offsets:
            best = [p for p in plist if p[0] <= want_off]
            print(f"  +{want_off}: " + (f"{best[-1][3]}.{best[-1][1]} ({best[-1][2]} at +{best[-1][0]})" if best else "-"))
        return
    pawn = next((o for o in objs if oname(o) == want), None)
    if not pawn:
        print("no object", want)
        return
    target = pawn
    if kind != "pawn":
        prop = "Controller" if kind == "controller" else "Mesh"
        for off, n, pc, owner in props(m.u32(pawn + CLASS)):
            if n == prop:
                target = m.u32(pawn + off)
                break
    cls = m.u32(target + CLASS)
    print(f"{kind} {target:08X} class {names(m.u32(cls + NAME))}")
    plist = props(cls)
    for want_off in offsets:
        best = [p for p in plist if p[0] <= want_off]
        if best:
            off, n, pc, owner = best[-1]
            print(f"  +{want_off}: {owner}.{n} ({pc} at +{off}, +{want_off - off} into it)")
        else:
            print(f"  +{want_off}: before the first property")


if __name__ == "__main__":
    main()
