import ctypes, ctypes.wintypes as w, time, sys, subprocess
# threadload.py <pid> [seconds]: CPU load of the busiest threads, with names (GetThreadDescription).
k=ctypes.windll.kernel32
pid=int(sys.argv[1]); secs=float(sys.argv[2]) if len(sys.argv)>2 else 3
class TE(ctypes.Structure):
    _fields_=[('dwSize',w.DWORD),('cntUsage',w.DWORD),('th32ThreadID',w.DWORD),('th32OwnerProcessID',w.DWORD),('tpBasePri',w.LONG),('tpDeltaPri',w.LONG),('dwFlags',w.DWORD)]
def tids():
    s=k.CreateToolhelp32Snapshot(4,0); e=TE(); e.dwSize=ctypes.sizeof(e); r=[]
    ok=k.Thread32First(s,ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID==pid: r.append(e.th32ThreadID)
        ok=k.Thread32Next(s,ctypes.byref(e))
    k.CloseHandle(s); return r
def info(t):
    h=k.OpenThread(0x0800|0x0040,False,t)
    if not h: return None,0
    a,b,c,d=w.FILETIME(),w.FILETIME(),w.FILETIME(),w.FILETIME()
    k.GetThreadTimes(h,ctypes.byref(a),ctypes.byref(b),ctypes.byref(c),ctypes.byref(d))
    cpu=((c.dwHighDateTime<<32|c.dwLowDateTime)+(d.dwHighDateTime<<32|d.dwLowDateTime))/1e4
    p=ctypes.c_wchar_p(); name=''
    if k.GetThreadDescription(h,ctypes.byref(p))>=0 and p.value: name=p.value
    k.CloseHandle(h); return name,cpu
t0={t:info(t) for t in tids()}
time.sleep(secs)
rows=[]
for t in tids():
    if t in t0:
        n,c=info(t); rows.append(((c-t0[t][1])/secs/10,n,t))
for r in sorted(rows,reverse=True)[:12]: print('%5.1f%%  %s (%d)'%r)
