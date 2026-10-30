"""UE3 Xenon package (LZO) reader: names, imports, exports. python upk.py <pkg> <out.bin>"""
import struct, sys


def lzo1x(src, out_len):
    dst = bytearray()
    ip = 0
    n = len(src)

    def length(base, mask):
        nonlocal ip
        t = src[ip - 1] & mask if False else None

    t = src[ip]
    state = 0
    if t > 17:
        ip += 1
        t -= 17
        dst += src[ip:ip + t]; ip += t
        state = 4 if t >= 4 else t
    while True:
        t = src[ip]; ip += 1
        if t < 16:
            if state == 0:
                if t == 0:
                    while src[ip] == 0:
                        t += 255; ip += 1
                    t += 15 + src[ip]; ip += 1
                t += 3
                dst += src[ip:ip + t]; ip += t
                state = 4
                continue
            elif state < 4:
                # 2-byte match, distance 1..1024
                d = (t >> 2) + (src[ip] << 2) + 1; ip += 1
                s = state = t & 3
                p = len(dst) - d
                for i in range(2):
                    dst.append(dst[p + i])
                dst += src[ip:ip + s]; ip += s
                continue
            else:
                d = (t >> 2) + (src[ip] << 2) + 2049; ip += 1
                p = len(dst) - d
                for i in range(3):
                    dst.append(dst[p + i])
                s = state = t & 3
                dst += src[ip:ip + s]; ip += s
                continue
        if t >= 64:
            ln = (t >> 5) + 1
            d = ((t >> 2) & 7) + (src[ip] << 3) + 1; ip += 1
        elif t >= 32:
            ln = t & 31
            if ln == 0:
                while src[ip] == 0:
                    ln += 255; ip += 1
                ln += 31 + src[ip]; ip += 1
            ln += 2
            v = src[ip] | (src[ip + 1] << 8); ip += 2
            d = (v >> 2) + 1
            t = v
        else:  # 16..31
            ln = t & 7
            hi = (t & 8) << 11
            if ln == 0:
                while src[ip] == 0:
                    ln += 255; ip += 1
                ln += 7 + src[ip]; ip += 1
            ln += 2
            v = src[ip] | (src[ip + 1] << 8); ip += 2
            d = hi + (v >> 2)
            if d == 0:
                break
            d += 16384
            t = v
        p = len(dst) - d
        for i in range(ln):
            dst.append(dst[p + i])
        s = state = t & 3
        dst += src[ip:ip + s]; ip += s
    return bytes(dst)


def read(path):
    f = open(path, 'rb').read()
    u32 = lambda o: struct.unpack_from('>I', f, o)[0]
    o = 12
    ln = struct.unpack_from('>i', f, o)[0]; o += 4 + ln
    o += 4  # flags
    namecount, nameoff, expcount, expoff, impcount, impoff = struct.unpack_from('>6I', f, o); o += 24
    o += 4 + 16
    gens = u32(o); o += 4 + 12 * gens
    o += 8
    comp = u32(o); o += 4
    nchunks = u32(o); o += 4
    total = 0
    chunks = []
    for i in range(nchunks):
        uo, us, co, cs = struct.unpack_from('>4I', f, o); o += 16
        chunks.append((uo, us, co, cs))
        total = max(total, uo + us)
    buf = bytearray(total)
    buf[:chunks[0][0]] = f[:chunks[0][0]]
    for uo, us, co, cs in chunks:
        p = co + 8
        csz, usz = struct.unpack_from('>2I', f, p); p += 8
        blocks = []
        rem = usz
        while rem > 0:
            bc, bu = struct.unpack_from('>2I', f, p); p += 8
            blocks.append((bc, bu)); rem -= bu
        q = uo
        for bc, bu in blocks:
            d = lzo1x(f[p:p + bc], bu); p += bc
            buf[q:q + len(d)] = d; q += len(d)
    return bytes(buf), (namecount, nameoff, expcount, expoff, impcount, impoff)


if __name__ == '__main__':
    data, hdr = read(sys.argv[1])
    open(sys.argv[2], 'wb').write(data)
    print(hdr, len(data))
