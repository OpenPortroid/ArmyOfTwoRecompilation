"""Dump guest memory from a running game process.

python memdump.py <pid> <host_virtual_base_hex> <guest_start_hex> <size_hex> <out_file>
Example: python memdump.py 1234 0x100000000 0x82000000 0x1200000 image.bin
"""
import ctypes
import ctypes.wintypes as wt
import sys

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


def main():
    pid = int(sys.argv[1])
    base = int(sys.argv[2], 16)
    start = int(sys.argv[3], 16)
    size = int(sys.argv[4], 16)
    out = sys.argv[5]
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")
    page = 0x10000
    data = bytearray(size)
    bad = 0
    for off in range(0, size, page):
        n = min(page, size - off)
        buf = (ctypes.c_char * n)()
        got = ctypes.c_size_t(0)
        if k32.ReadProcessMemory(h, ctypes.c_void_p(base + start + off), buf, n, ctypes.byref(got)):
            data[off:off + n] = buf.raw
        else:
            bad += 1
    open(out, "wb").write(data)
    print(f"wrote {out}: {size:#x} bytes, unreadable 64K pages: {bad}")


if __name__ == "__main__":
    main()
