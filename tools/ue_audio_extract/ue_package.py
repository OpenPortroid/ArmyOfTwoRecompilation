#!/usr/bin/env python3
"""ue_package.py

UE3 (CookedXenon) container helpers for LOC packages of the 2008 Army of Two.

What lives here:

* `parse_chunk_table(path)` — read the compressed-chunk table of a package
  using the bundled C# tool `chunk_dump.exe` (UELib summary reader).
* `get_summary(path)` — name/import/export table offsets.
* `decompress_chunks(path)` — concatenate all chunks' decompressed bytes in
  logical order (LZXP/LZO chunk decoder, see ``lzodec.cpp``).
* `assemble_package(src, dst)` — build a plain (uncompressed) UE3 package:
  first 101 header bytes + concatenated payloads, with the CompressionFlags
  word zeroed out so plain readers (UELib) can continue past the summary.

Verified facts about this game's packages:
- Header: signature at [0:4], version/headers, NameTable at offset 101.
- CompressionFlags==2 → LZXP/LZO compressed chunks. UELib Deserialize()
  returns early when compressed chunks are present or Classification flags
  are set (``We can't continue without decompressing``), leaving
  Names/Exports/Imports empty. That is library behavior, not an API bug.
- Chunk table entries (UELib ReadArray order) and how to map them to
  (LogicalOffset, UncompressedSize, CompressedOffset, CompressedSize):
    dump prints `CHUNK uOff=.. uSize=.. cOff=.. cSize=..` where
    uOff=UncompressedOffset, uSize actually holds CompressedOffset and
    cOff holds UncompressedSize? Verified empirically (market LOC file):
      uncompressed chunk size == the .xxx header's uncompressed size, and
      CompressedOffset == offset of the on-disk CompressedChunkHeader.
"""
import re
import subprocess
import os

LATEST_CHUNK_FLAG = b"\x00\x00\x00\x02"

HERE = os.path.dirname(os.path.abspath(__file__))
CHUNKDUMP = os.environ.get("UE_CHUNK_DUMP") or os.path.join(HERE, "..", "chunk_dump.exe")
LZODEC = os.environ.get("UE_LZODEC") or os.path.join(HERE, "lzodec.exe")


def parse_chunk_table(path):
    out = subprocess.run([CHUNKDUMP, path], capture_output=True, text=True)
    chunks = []
    for line in (out.stdout + out.stderr).splitlines():
        m = re.match(r"CHUNK uOff=(\d+) uSize=(\d+) cOff=(\d+) cSize=(\d+)", line.strip())
        if not m:
            continue
        u_off, u_size, c_off, c_size = map(int, m.groups())
        chunks.append((u_off, c_off, u_size, c_size))
    chunks.sort(key=lambda c: c[0])
    return chunks


def get_summary(path):
    out = subprocess.run([CHUNKDUMP, path], capture_output=True, text=True)
    txt = out.stdout + out.stderr
    m = re.search(
        r"Names Count:(\d+) Names Offset:(\d+)\s+Exports Count:(\d+) Exports Offset:(\d+)"
        r"\s+Imports Count:(\d+) Imports Offset:(\d+)",
        txt,
    )
    if not m:
        raise RuntimeError("could not read summary:\n" + txt[:600])
    names_c, names_o, exps_c, exps_o, imps_c, imps_o = map(int, m.groups())
    return {
        "names": (names_c, names_o),
        "exports": (exps_c, exps_o),
        "imports": (imps_c, imps_o),
    }


def decompress_chunks(path):
    """Returns the concatenated decompressed payload (bytes) of a package.
    Each chunk is decompressed with ``lzodec`` (independent LZO1X blocks)."""
    chunks = parse_chunk_table(path)
    if not chunks:
        raise RuntimeError("no compressed chunks in " + path)
    parts = []
    for i, (uoff, uncomp, coff, csize) in enumerate(chunks):
        tmp = "/tmp/_ue_chunk_%d.raw" % i
        rr = subprocess.run([LZODEC, path, tmp, str(coff)], capture_output=True, text=True)
        if rr.returncode != 0:
            raise RuntimeError("lzodec failed chunk %d: %s" % (i, rr.stderr))
        raw = open(tmp, "rb").read()
        assert len(raw) == uncomp, "chunk %d size %d != %d" % (i, len(raw), uncomp)
        parts.append(raw)
    return b"".join(parts), (chunks[0][0], chunks[-1][0] + chunks[-1][1])


def assemble_package(src, dst):
    """Build the plain (uncompressed) package from the compressed source."""
    payload, (u0, u1) = decompress_chunks(src)
    d = open(src, "rb").read()
    assembled = bytearray(d[:101] + payload)
    # Logical length check
    pf = d.find(LATEST_CHUNK_FLAG + b"\x00\x00\x00", 0, 2000)
    if pf >= 0:
        assembled[pf:pf + 4] = b"\x00\x00\x00\x00"
    open(dst, "wb").write(assembled)
    return dst
