"""Sampling profiler for one thread of a running process (Windows x64).

Usage: python sampler.py <pid> list  (thread ids and names)
       (a thread is chosen by a substring of its name, or by its id)
       python sampler.py <pid> <thread name substring> [seconds=10] [hz=1000] [--top N] [--tree DEPTH]
              [--lines]  (self time by innermost inlined function and source line)

Suspends the thread ~hz times per second, walks its stack (dbghelp
StackWalk64 with the modules' unwind info), and prints:
  - self time per function (leaf frames),
  - inclusive time per function (any frame),
symbolized with llvm-symbolizer (PDB next to the exe). Thread names come from
GetThreadDescription (XHostThread names, e.g. "Native GPU commands").
"""
import collections
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)

PROCESS_ALL = 0x0400 | 0x0010  # QUERY_INFORMATION | VM_READ
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040 | 0x0800  # SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION | QUERY_LIMITED
TH32CS_SNAPTHREAD = 0x4
CONTEXT_FULL = 0x10000B
SYMBOLIZER = r"C:\Program Files\LLVM\bin\llvm-symbolizer.exe"


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", wt.WORD), ("Mode", wt.DWORD)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64), ("FuncTableEntry", ctypes.c_void_p),
                ("Params", ctypes.c_uint64 * 4), ("Far", wt.BOOL), ("Virtual", wt.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", ctypes.c_byte * 512)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wt.DWORD), ("EntryPoint", ctypes.c_void_p)]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
dbghelp.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbghelp.SymFunctionTableAccess64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.SymGetModuleBase64.restype = ctypes.c_uint64
dbghelp.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_uint64]
FTA = ctypes.WINFUNCTYPE(ctypes.c_void_p, wt.HANDLE, ctypes.c_uint64)
GMB = ctypes.WINFUNCTYPE(ctypes.c_uint64, wt.HANDLE, ctypes.c_uint64)
dbghelp.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64), ctypes.c_void_p,
                                ctypes.c_void_p, FTA, GMB, ctypes.c_void_p]


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return out


def modules_of(hproc):
    arr = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(hproc, arr, ctypes.sizeof(arr), ctypes.byref(needed), 3)
    mods = []
    for i in range(needed.value // ctypes.sizeof(ctypes.c_void_p)):
        info = MODULEINFO()
        psapi.GetModuleInformation(hproc, ctypes.c_void_p(arr[i]), ctypes.byref(info), ctypes.sizeof(info))
        name = ctypes.create_unicode_buffer(520)
        psapi.GetModuleFileNameExW(hproc, ctypes.c_void_p(arr[i]), name, 520)
        mods.append((info.lpBaseOfDll, info.SizeOfImage, name.value))
    return mods


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = sys.argv[1:]
    pid = int(args[0])
    want = args[1].lower()
    seconds = float(args[2]) if len(args) > 2 else 10.0
    hz = float(args[3]) if len(args) > 3 else 1000.0
    top = int(opts[opts.index("--top") + 1]) if "--top" in opts else 40

    hproc = k32.OpenProcess(PROCESS_ALL, False, pid)
    if want == "list":
        for tid in threads_of(pid):
            h = k32.OpenThread(THREAD_ACCESS, False, tid)
            desc = ctypes.c_wchar_p()
            name = desc.value if h and k32.GetThreadDescription(h, ctypes.byref(desc)) >= 0 else None
            print(tid, name or "")
        return
    target = None
    for tid in threads_of(pid):
        h = k32.OpenThread(THREAD_ACCESS, False, tid)
        if not h:
            continue
        desc = ctypes.c_wchar_p()
        named = k32.GetThreadDescription(h, ctypes.byref(desc)) >= 0 and desc.value
        if (want.isdigit() and tid == int(want)) or (named and want in desc.value.lower()):
            target = (tid, h, desc.value if named else "")
            break
        k32.CloseHandle(h)
    if not target:
        raise SystemExit(f"no thread named like '{want}'")
    tid, hthread, tname = target
    dbghelp.SymSetOptions(0x4)  # SYMOPT_DEFERRED_LOADS
    dbghelp.SymInitialize(hproc, None, True)
    fta = FTA(lambda hp, a: dbghelp.SymFunctionTableAccess64(hp, a))
    gmb = GMB(lambda hp, a: dbghelp.SymGetModuleBase64(hp, a))

    raw = ctypes.create_string_buffer(1232 + 16)
    ctx_addr = (ctypes.addressof(raw) + 15) & ~15
    stacks = []
    interval = 1.0 / hz
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        if k32.SuspendThread(hthread) == 0xFFFFFFFF:
            break
        try:
            ctypes.memset(ctx_addr, 0, 1232)
            ctypes.c_uint32.from_address(ctx_addr + 0x30).value = CONTEXT_FULL
            if not k32.GetThreadContext(hthread, ctypes.c_void_p(ctx_addr)):
                continue
            rip = ctypes.c_uint64.from_address(ctx_addr + 0xF8).value
            rsp = ctypes.c_uint64.from_address(ctx_addr + 0x98).value
            rbp = ctypes.c_uint64.from_address(ctx_addr + 0xA0).value
            frame = STACKFRAME64()
            frame.AddrPC.Offset, frame.AddrPC.Mode = rip, 3
            frame.AddrStack.Offset, frame.AddrStack.Mode = rsp, 3
            frame.AddrFrame.Offset, frame.AddrFrame.Mode = rbp, 3
            pcs = []
            for _ in range(48):
                if not dbghelp.StackWalk64(0x8664, hproc, hthread, ctypes.byref(frame), ctypes.c_void_p(ctx_addr),
                                           None, fta, gmb, None):
                    break
                if not frame.AddrPC.Offset:
                    break
                pcs.append(frame.AddrPC.Offset)
            stacks.append(pcs or [rip])
        finally:
            k32.ResumeThread(hthread)
        time.sleep(interval)

    mods = modules_of(hproc)

    def locate(a):
        for base, size, name in mods:
            if base <= a < base + size:
                return name, a - base
        return None, a

    uniq = sorted({a for s in stacks for a in s})
    by_mod = collections.defaultdict(list)
    for a in uniq:
        name, off = locate(a)
        by_mod[name].append((a, off))
    names = {}
    for mod, items in by_mod.items():
        if not mod or not os.path.exists(mod) or not os.path.exists(os.path.splitext(mod)[0] + ".pdb"):
            for a, off in items:
                names[a] = f"{os.path.basename(mod) if mod else '?'}+{off:#x}"
            continue
        inp = "\n".join(f"{off:#x}" for _, off in items) + "\n"
        res = subprocess.run([SYMBOLIZER, f"--obj={mod}", "--relative-address", "--no-inlines", "--functions=linkage",
                              "--output-style=GNU"], input=inp, capture_output=True, text=True)
        lines = res.stdout.split("\n")
        # GNU style: function\nfile:line per address.
        for i, (a, off) in enumerate(items):
            fn = lines[2 * i] if 2 * i < len(lines) else "?"
            names[a] = fn if fn and fn != "??" else f"{os.path.basename(mod)}+{off:#x}"

    n = len(stacks)
    self_c = collections.Counter(names[s[0]] for s in stacks)
    incl = collections.Counter()
    for s in stacks:
        for fn in {names[a] for a in s}:
            incl[fn] += 1
    print(f"thread {tid} '{tname}': {n} samples in {seconds:.0f}s")
    print("--- self ---")
    for fn, c in self_c.most_common(top):
        print(f"{100.0 * c / n:6.2f}%  {fn}")
    print("--- inclusive ---")
    for fn, c in incl.most_common(top):
        print(f"{100.0 * c / n:6.2f}%  {fn}")
    if "--ours" in opts:
        # Leaf module/function attributed to the nearest frame of non-DLL code.
        paths = collections.Counter()
        for s in stacks:
            leaf = names[s[0]]
            leaf = leaf.split("+")[0] if ".dll+" in leaf else leaf
            caller = next((names[a] for a in s if ".dll+" not in names[a] and "NativeLog" not in names[a]), "?")
            paths[f"{leaf} <= {caller}"] += 1
        print("--- leaf <= first own frame ---")
        for p, c in paths.most_common(top):
            print(f"{100.0 * c / n:6.2f}%  {p}")
    if "--ranges" in opts:
        # Inclusive share of guest address ranges given as --ranges lo-hi:name,lo-hi:name (hex).
        spec = opts[opts.index("--ranges") + 1]
        import re
        for part in spec.split(","):
            rng, label = part.split(":")
            lo, hi = (int(x, 16) for x in rng.split("-"))
            def hit(name):
                m = re.search(r"sub_([0-9A-F]{8})", name)
                return m and lo <= int(m.group(1), 16) < hi
            c = sum(1 for st in stacks if any(hit(names[a]) for a in st))
            print(f"{100.0 * c / n:6.2f}%  inclusive {label} ({lo:08X}-{hi:08X})")
    if "--lines" in opts:
        # Leaf samples of modules with a PDB by innermost inlined function and source line.
        leaf_set = {s[0] for s in stacks}
        inl = {}
        for mod, items in by_mod.items():
            if not mod or not os.path.exists(os.path.splitext(mod)[0] + ".pdb"):
                continue
            leafs = [(a, off) for a, off in items if a in leaf_set]
            inp = "\n".join(f"{off:#x}" for _, off in leafs) + "\n"
            res = subprocess.run([SYMBOLIZER, f"--obj={mod}", "--relative-address", "--inlines",
                                  "--functions=short"], input=inp, capture_output=True, text=True)
            for (a, off), block in zip(leafs, res.stdout.split("\n\n")):
                ls = [l for l in block.strip().split("\n") if l]
                if len(ls) >= 2:
                    line = ls[1].rsplit(":", 1)[0]
                    inl[a] = (ls[0], f"{ls[0]} @ {os.path.basename(line)}")
        by_fn = collections.Counter(inl[s[0]][0] for s in stacks if s[0] in inl)
        by_line = collections.Counter(inl[s[0]][1] for s in stacks if s[0] in inl)
        print("--- innermost inlined function (self) ---")
        for p, c in by_fn.most_common(top):
            print(f"{100.0 * c / n:6.2f}%  {p}")
        print("--- source lines (self) ---")
        for p, c in by_line.most_common(top):
            print(f"{100.0 * c / n:6.2f}%  {p}")
    if "--stacks" in opts:
        # Most common call paths (leaf first), cut at the first frame of our own code.
        depth = int(opts[opts.index("--stacks") + 1])
        paths = collections.Counter(" <- ".join(names[a] for a in s[:depth]) for s in stacks)
        print("--- stacks ---")
        for p, c in paths.most_common(top):
            print(f"{100.0 * c / n:6.2f}%  {p}")


if __name__ == "__main__":
    main()
