import ctypes, ctypes.wintypes as w, time, sys, datetime
# threadseries.py <pid> <seconds> [min %]: CPU load of each named thread once a second, with the local time
# (the [HH:MM:SS] of decouple.log), so a slow second can be matched with the thread that was full in it.
# One line per second: time, then "name pct" for the threads above min % (default 5).
k = ctypes.windll.kernel32
pid = int(sys.argv[1]); secs = int(sys.argv[2]); floor = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0


class TE(ctypes.Structure):
    _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('th32ThreadID', w.DWORD), ('th32OwnerProcessID', w.DWORD),
                ('tpBasePri', w.LONG), ('tpDeltaPri', w.LONG), ('dwFlags', w.DWORD)]


def tids():
    s = k.CreateToolhelp32Snapshot(4, 0); e = TE(); e.dwSize = ctypes.sizeof(e); r = []
    ok = k.Thread32First(s, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid: r.append(e.th32ThreadID)
        ok = k.Thread32Next(s, ctypes.byref(e))
    k.CloseHandle(s); return r


handles = {}


def cpu(t):
    if t not in handles:
        h = k.OpenThread(0x0800 | 0x0040, False, t)
        if not h: return None, 0
        p = ctypes.c_wchar_p(); name = ''
        if k.GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value: name = p.value
        handles[t] = (h, name or str(t))
    h, name = handles[t]
    a, b, c, d = w.FILETIME(), w.FILETIME(), w.FILETIME(), w.FILETIME()
    if not k.GetThreadTimes(h, ctypes.byref(a), ctypes.byref(b), ctypes.byref(c), ctypes.byref(d)): return None, 0
    return name, ((c.dwHighDateTime << 32 | c.dwLowDateTime) + (d.dwHighDateTime << 32 | d.dwLowDateTime)) / 1e4


last = {t: cpu(t)[1] for t in tids()}
t_last = time.perf_counter()
for _ in range(secs):
    time.sleep(1.0 - (time.perf_counter() - t_last) % 1.0)
    now = time.perf_counter(); dt = now - t_last; t_last = now
    row = []
    for t in tids():
        name, c = cpu(t)
        if name is None: continue
        if t in last: row.append(((c - last[t]) / dt / 10, name))
        last[t] = c
    stamp = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
    print('[%s] ' % stamp + ' | '.join('%s %.0f' % (n, p) for p, n in sorted(row, reverse=True) if p >= floor), flush=True)
