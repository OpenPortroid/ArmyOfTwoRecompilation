#!/usr/bin/env python3
"""Makes shader_manifest.bin (shipped with the release) from pipelines.bin.

pipelines.bin (shader_cache/ in the game folder) lists every shader variant
and pipeline the game used, with the shaders' microcode: game code, so it is
never shipped. The manifest keeps only the microcode hashes, the variants
(modification) and the pipeline states; the game supplies the microcode itself
when it creates its shaders while loading (src/gpu/native/shader_capture.cpp,
"Shader manifest" in renderer.cpp). Vertex shaders also get the vertex fetch
instructions as the D3D binds them (vertex format, stride, offset): the created
microcode has other ones there, it is found by its hash without them.

    python tools/make_shader_manifest.py OUT pipelines.bin [more pipelines.bin ...]

Several inputs (e.g. from different playthroughs) are merged.
"""
import struct
import sys

STORAGE_MAGIC = 0x50324F41  # "AO2P"
STORAGE_VERSION = 1
MANIFEST_MAGIC = 0x4D324F41  # "AO2M"
MANIFEST_VERSION = 2
VERTEX = 0  # xenos::ShaderType::kVertex


def fnv(raw_dwords):
    """HashDwords (src/gpu/native/pm4.cpp) over dwords as stored in memory."""
    h = 0xCBF29CE484222325 ^ len(raw_dwords)
    for v in raw_dwords:
        h = ((h ^ v) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def fetch_dwords(d):
    """capture::FetchDwords: indices of the vertex fetch instructions' dwords.
    d: the dwords' values (big-endian decoded)."""
    slots = len(d) // 3
    cf_end = slots
    out = set()
    i = 0
    while i < cf_end * 2 and i // 2 < slots:
        s = (i // 2) * 3
        if i % 2 == 0:
            ins = d[s] | (d[s + 1] & 0xFFFF) << 32
        else:
            ins = (d[s + 1] >> 16) | d[s + 2] << 16
        i += 1
        op = (ins >> 44) & 0xF
        if not (1 <= op <= 6 or op in (13, 14)):
            continue
        address, clause, sequence = ins & 0xFFF, (ins >> 12) & 7, (ins >> 16) & 0xFFF
        if address and address < cf_end:
            cf_end = address
        for k in range(clause):
            a = (address + k) * 3
            if (sequence >> (2 * k)) & 1 and a + 2 < len(d) and (d[a] & 0x1F) == 0:
                out.update((a, a + 1, a + 2))
    return sorted(out)


def read_storage(path, key_size, variants, vertex, pipelines):
    data = open(path, 'rb').read()
    magic, version, size = struct.unpack_from('<3I', data, 0)
    if magic != STORAGE_MAGIC or version != STORAGE_VERSION:
        sys.exit(f'{path}: not a pipelines.bin (version {STORAGE_VERSION})')
    if key_size is not None and size != key_size:
        sys.exit(f'{path}: pipeline key of {size} bytes, others have {key_size}')
    pos = 12
    shaders = pipes = 0
    while pos < len(data):
        tag = data[pos]
        pos += 1
        if tag == ord('S'):
            if pos + 21 > len(data):
                break
            shader_type = data[pos]
            hash_, modification, count = struct.unpack_from('<QQI', data, pos + 1)
            code = data[pos + 21:pos + 21 + 4 * count]
            pos += 21 + 4 * count
            if pos > len(data):
                break
            variants.setdefault((shader_type, hash_, modification), None)
            if shader_type == VERTEX and hash_ not in vertex:
                raw = struct.unpack('<%dI' % count, code)
                fetch = fetch_dwords(struct.unpack('>%dI' % count, code))
                masked = list(raw)
                for i in fetch:
                    masked[i] = 0
                vertex[hash_] = (fnv(masked), [(i, raw[i]) for i in fetch])
            shaders += 1
        elif tag == ord('P'):
            if pos + 32 + size > len(data):
                break
            record = data[pos:pos + 32 + size]
            pos += 32 + size
            pipelines.setdefault(record, None)
            pipes += 1
        else:
            break
    print(f'{path}: {shaders} shaders, {pipes} pipelines')
    return size


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = sys.argv[1]
    variants, vertex, pipelines = {}, {}, {}  # ordered
    key_size = None
    for path in sys.argv[2:]:
        key_size = read_storage(path, key_size, variants, vertex, pipelines)
    with open(out, 'wb') as f:
        f.write(struct.pack('<3I', MANIFEST_MAGIC, MANIFEST_VERSION, key_size))
        for shader_type, hash_, modification in variants:
            f.write(b'H' + struct.pack('<BQQ', shader_type, hash_, modification))
        for hash_, (masked, fetch) in vertex.items():
            f.write(b'V' + struct.pack('<QQH', hash_, masked, len(fetch)))
            for index, value in fetch:
                f.write(struct.pack('<HI', index, value))
        for record in pipelines:
            f.write(b'P' + record)
    print(f'{out}: {len(variants)} shader variants, {len(vertex)} vertex shaders, {len(pipelines)} pipelines')


if __name__ == '__main__':
    main()
