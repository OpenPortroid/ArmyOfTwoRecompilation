"""One-shot stacks of every thread of a running process (hangs/deadlocks).

Usage: python threadstacks.py <pid> [--all] [--depth N]
Prints each thread (id, name) with its symbolized stack; without --all only
threads that run guest code (a sub_XXXXXXXX frame) are shown.
"""
import collections
import ctypes
import os
import subprocess
import sys

import sampler as s


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = sys.argv[1:]
    pid = int(args[0])
    depth = int(opts[opts.index("--depth") + 1]) if "--depth" in opts else 24
    hproc = s.k32.OpenProcess(s.PROCESS_ALL, False, pid)
    s.dbghelp.SymSetOptions(0x4)
    s.dbghelp.SymInitialize(hproc, None, True)
    fta = s.FTA(lambda hp, a: s.dbghelp.SymFunctionTableAccess64(hp, a))
    gmb = s.GMB(lambda hp, a: s.dbghelp.SymGetModuleBase64(hp, a))
    raw = ctypes.create_string_buffer(1232 + 16)
    ctx_addr = (ctypes.addressof(raw) + 15) & ~15
    threads = []
    for tid in s.threads_of(pid):
        h = s.k32.OpenThread(s.THREAD_ACCESS, False, tid)
        if not h:
            continue
        desc = ctypes.c_wchar_p()
        name = desc.value if s.k32.GetThreadDescription(h, ctypes.byref(desc)) >= 0 else ""
        if s.k32.SuspendThread(h) == 0xFFFFFFFF:
            continue
        pcs = []
        try:
            ctypes.memset(ctx_addr, 0, 1232)
            ctypes.c_uint32.from_address(ctx_addr + 0x30).value = s.CONTEXT_FULL
            if s.k32.GetThreadContext(h, ctypes.c_void_p(ctx_addr)):
                frame = s.STACKFRAME64()
                frame.AddrPC.Offset, frame.AddrPC.Mode = ctypes.c_uint64.from_address(ctx_addr + 0xF8).value, 3
                frame.AddrStack.Offset, frame.AddrStack.Mode = ctypes.c_uint64.from_address(ctx_addr + 0x98).value, 3
                frame.AddrFrame.Offset, frame.AddrFrame.Mode = ctypes.c_uint64.from_address(ctx_addr + 0xA0).value, 3
                for _ in range(64):
                    if not s.dbghelp.StackWalk64(0x8664, hproc, h, ctypes.byref(frame), ctypes.c_void_p(ctx_addr),
                                                 None, fta, gmb, None) or not frame.AddrPC.Offset:
                        break
                    pcs.append(frame.AddrPC.Offset)
        finally:
            s.k32.ResumeThread(h)
        threads.append((tid, name or "", pcs))

    mods = s.modules_of(hproc)

    def locate(a):
        for base, size, name in mods:
            if base <= a < base + size:
                return name, a - base
        return None, a

    by_mod = collections.defaultdict(set)
    for _, _, pcs in threads:
        for a in pcs:
            by_mod[locate(a)[0]].add(a)
    names = {}
    for mod, addrs in by_mod.items():
        items = sorted(addrs)
        pdb = mod and os.path.exists(os.path.splitext(mod)[0] + ".pdb")
        if not pdb:
            for a in items:
                names[a] = f"{os.path.basename(mod) if mod else '?'}+{locate(a)[1]:#x}"
            continue
        inp = "\n".join(f"{locate(a)[1]:#x}" for a in items) + "\n"
        res = subprocess.run([s.SYMBOLIZER, f"--obj={mod}", "--relative-address", "--no-inlines",
                              "--functions=linkage", "--output-style=GNU"], input=inp, capture_output=True, text=True)
        lines = res.stdout.split("\n")
        for i, a in enumerate(items):
            fn = lines[2 * i] if 2 * i < len(lines) else "?"
            names[a] = fn if fn and fn != "??" else f"{os.path.basename(mod)}+{locate(a)[1]:#x}"

    for tid, name, pcs in threads:
        frames = [names[a] for a in pcs]
        if "--all" not in opts and not any(f.startswith("sub_") for f in frames):
            continue
        print(f"=== {tid} {name}")
        print("   " + " <- ".join(frames[:depth]))


if __name__ == "__main__":
    main()
