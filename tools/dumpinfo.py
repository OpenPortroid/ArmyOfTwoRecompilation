"""Minimal minidump reader: exception code/address + faulting module offset.

Usage: python dumpinfo.py <dump.dmp> [--threads] [--exe <armyoftworecomp.exe>]
Prints the exception record, the module containing the address, and
stack-scanned return addresses that fall inside known modules (for
llvm-symbolizer: --obj=<module> <offset>).
--threads: every thread (hang dumps, hang_*.dmp): name and stack-scanned
frames of our exe, symbolized with llvm-symbolizer and the exe's PDB (the
exe of the same build: --exe, default the game folder's).
"""
import os
import struct
import subprocess
import sys

path = sys.argv[1]
data = open(path, 'rb').read()
sig, ver, nstreams, dir_rva = struct.unpack_from('<IIII', data, 0)
streams = {}
for i in range(nstreams):
    stype, size, rva = struct.unpack_from('<III', data, dir_rva + i * 12)
    streams.setdefault(stype, []).append((size, rva))


def read_str(rva):
    n = struct.unpack_from('<I', data, rva)[0]
    return data[rva + 4:rva + 4 + n].decode('utf-16-le')


modules = []
for size, rva in streams.get(4, []):  # ModuleListStream
    n = struct.unpack_from('<I', data, rva)[0]
    for i in range(n):
        off = rva + 4 + i * 108
        base, msize = struct.unpack_from('<QI', data, off)
        name_rva = struct.unpack_from('<I', data, off + 20)[0]
        modules.append((base, msize, read_str(name_rva)))


def find_mod(addr):
    for base, msize, name in modules:
        if base <= addr < base + msize:
            return name, addr - base
    return None, None


mem = []
for size, rva in streams.get(9, []):  # Memory64ListStream
    n, base_rva = struct.unpack_from('<QQ', data, rva)
    cur = base_rva
    for i in range(n):
        start, msize = struct.unpack_from('<QQ', data, rva + 16 + i * 16)
        mem.append((start, msize, cur))
        cur += msize
for size, rva in streams.get(5, []):  # MemoryListStream
    n = struct.unpack_from('<I', data, rva)[0]
    for i in range(n):
        start, dsize, drva = struct.unpack_from('<QII', data, rva + 4 + i * 16)
        mem.append((start, dsize, drva))


def read_mem(addr, n):
    for start, msize, off in mem:
        if start <= addr and addr + n <= start + msize:
            return data[off + addr - start: off + addr - start + n]
    return None


for size, rva in streams.get(6, []):  # ExceptionStream
    tid = struct.unpack_from('<I', data, rva)[0]
    code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', data, rva + 8)
    params = struct.unpack_from('<15Q', data, rva + 8 + 32)
    ctx_size, ctx_rva = struct.unpack_from('<II', data, rva + 8 + 152)
    print(f'thread {tid} code=0x{code:08X} addr=0x{addr:X} params={[hex(p) for p in params[:nparams]]}')
    print('  in', find_mod(addr))
    rsp = struct.unpack_from('<Q', data, ctx_rva + 0x98)[0]
    rip = struct.unpack_from('<Q', data, ctx_rva + 0xF8)[0]
    print(f'  rip=0x{rip:X} rsp=0x{rsp:X}', find_mod(rip))
    stack = b''
    for start, msize, off in mem:
        if start <= rsp < start + msize:
            n = min(0x8000, start + msize - rsp)
            stack = data[off + rsp - start: off + rsp - start + n]
            break
    shown = 0
    for i in range(0, len(stack), 8):
        v = struct.unpack_from('<Q', stack, i)[0]
        name, off = find_mod(v)
        if name and ('armyoftwo' in name or 'rex' in name):
            print(f'  [rsp+0x{i:X}] {name.split(chr(92))[-1]}+0x{off:X}')
            shown += 1
            if shown > 80:
                break


def thread_names():
    names = {}
    for size, rva in streams.get(24, []):  # ThreadNamesStream
        n = struct.unpack_from('<I', data, rva)[0]
        for i in range(n):
            tid, name_rva = struct.unpack_from('<IQ', data, rva + 4 + i * 12)
            names[tid] = read_str(name_rva)
    return names


def symbolize(exe, offsets):
    symbolizer = r"C:\Program Files\LLVM\bin\llvm-symbolizer.exe"
    if not offsets or not os.path.exists(exe) or not os.path.exists(symbolizer):
        return {}
    inp = "\n".join(f"{o:#x}" for o in offsets) + "\n"
    res = subprocess.run([symbolizer, f"--obj={exe}", "--relative-address", "--no-inlines", "--functions=linkage",
                          "--output-style=GNU"], input=inp, capture_output=True, text=True)
    lines = res.stdout.split("\n")
    return {o: lines[2 * i] for i, o in enumerate(offsets) if 2 * i < len(lines) and lines[2 * i] != "??"}


if '--threads' in sys.argv:
    opts = sys.argv[1:]
    exe = opts[opts.index('--exe') + 1] if '--exe' in opts else \
        r"C:\aot2\aot\out\build\win-amd64-release\armyoftworecomp.exe"
    names = thread_names()
    threads = []
    for size, rva in streams.get(3, []):  # ThreadListStream
        n = struct.unpack_from('<I', data, rva)[0]
        for i in range(n):
            off = rva + 4 + i * 48
            tid = struct.unpack_from('<I', data, off)[0]
            st_start, st_size, st_rva = struct.unpack_from('<QII', data, off + 24)
            ctx_size, ctx_rva = struct.unpack_from('<II', data, off + 40)
            rsp = struct.unpack_from('<Q', data, ctx_rva + 0x98)[0]
            rip = struct.unpack_from('<Q', data, ctx_rva + 0xF8)[0]
            frames = [rip]
            if st_start <= rsp < st_start + st_size:
                begin = rsp - st_start
                for p in range(begin, min(st_size, begin + 0x10000) - 7, 8):
                    frames.append(struct.unpack_from('<Q', data, st_rva + p)[0])
            threads.append((tid, names.get(tid, ''), frames))
    ours = set()
    for _, _, frames in threads:
        for a in frames:
            name, off = find_mod(a)
            if name and name.lower().endswith('armyoftworecomp.exe'):
                ours.add(off)
    syms = symbolize(exe, sorted(ours))
    for tid, tname, frames in threads:
        print(f"=== thread {tid} '{tname}'")
        shown = 0
        for a in frames:
            name, off = find_mod(a)
            if not name:
                continue
            base = name.split(chr(92))[-1]
            if base.lower() == 'armyoftworecomp.exe':
                label = syms.get(off, f"{base}+{off:#x}")
            elif a == frames[0] or 'rex' in base.lower():
                label = f"{base}+{off:#x}"
            else:
                continue
            print(f"  {label}")
            shown += 1
            if shown >= 40:
                break
