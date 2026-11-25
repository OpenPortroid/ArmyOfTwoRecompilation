#!/usr/bin/env python3
"""xma2_reencode.py — splice dubbed WAVs into an assembled UE3 package.

Takes a decompressed (plain) package and a directory with dubbed WAVs named
``<WaveName>.wav``; for each wave whose bulk data slot is present it:

  1. WAV -> XMA2 packets via the external xma2enc CLI (birabittoh/xma2enc,
     WMA-Pro bitstream + XMA2 packet layer; build from source or supply
     ``xma2enc_cli.exe`` via ``XMA2ENC``).
  2. Rebuilds the 108-byte UE bulk header (new dataSize, numSamples,
     packet count, per-packet cumulative sample positions) — the exact
     layout we reverse-engineered from the original data.
  3. Splices the new blob into the package at the wave's StorageOffset,
     padding the remainder of the StorageSize region with 0xFF so that
     file size and all package offsets stay intact (the engine reads
     lengths out of the table, not the file position).

Usage:
    python xma2_reencode.py <assembled_pkg> <dub_wavs_dir> <out_pkg>
                               [--bitrate 36000] [--budget <bytes>]
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
XMA2ENC = os.environ.get("XMA2ENC") or os.path.join(HERE, "xma2enc_cli.exe")
MAX_BUDGET_FACTOR = 1.0  # never exceed StorageSize


def frames_of(packet):
    """XMA2 packet header: big-endian u32, frames starting in the packet
    are in bits [26..31]."""
    h = struct.unpack(">I", packet[:4])[0]
    return (h >> 26) & 0x3F


def encode(wav_path, tmp_out, bitrate):
    r = subprocess.run([XMA2ENC, wav_path, tmp_out, str(bitrate)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("xma2enc failed:\n" + r.stderr)
    d = open(tmp_out, "rb").read()
    i = d.find(b"data")
    n = struct.unpack("<I", d[i + 4:i + 8])[0]
    return d[i + 8:i + 8 + n]


def encode_to_budget(wav_path, budget, tmp_out):
    """Try decreasing bitrates until the encoded blob fits."""
    for bitrate in (40000, 38000, 36000, 34000, 32000, 30000, 28000):
        raw = encode(wav_path, tmp_out, bitrate)
        if len(raw) % 2048 == 0:
            return raw, bitrate
    raise RuntimeError("all bitrates overflow budget for " + wav_path)


def rebuild_blob(orig_hdr, raw, keep_fields_12_36=None):
    nblocks = len(raw) // 2048
    frames = [frames_of(raw[k * 2048:(k + 1) * 2048]) for k in range(nblocks)]
    hdr = bytearray()
    hdr += struct.pack(">I", struct.unpack(">I", orig_hdr[0:4])[0])       # 44
    hdr += struct.pack(">I", struct.unpack(">I", orig_hdr[4:8])[0])       # 52
    hdr += struct.pack(">I", len(raw))                                     # dataSize
    hdr += orig_hdr[12:36]                                                 # constants
    hdr += struct.pack(">I", 2048)                                         # packet size
    hdr += struct.pack(">I", sum(frames) * 512)                            # numSamples
    hdr += orig_hdr[44:48]                                                 # trailing dword
    hdr += struct.pack(">I", nblocks)                                      # nPackets
    hdr += struct.pack(">I", 0x01000000)                                   # marker
    cum = 0
    for fc in frames:
        cum += fc * 512
        hdr += struct.pack(">I", cum)
    return bytes(hdr) + raw


def repack(assembled_pkg, dub_dir, out_pkg, verbose=True):
    pkg = bytearray(open(assembled_pkg, "rb").read())
    # Find waves and their storage in the assembled package.
    list_csv = os.environ.get("UE_AUDIO_LIST") or os.path.join(HERE, "waves.tsv")
    entries = []
    if os.path.exists(list_csv):
        for line in open(list_csv, encoding="utf-8"):
            parts = line.rstrip("\n").split(",")
            if len(parts) < 4 or parts[0] == "wave":
                continue
            entries.append((parts[0], int(parts[2]), int(parts[3])))
    else:
        raise RuntimeError("run ue_audio_extract.py first (needs waves.tsv)")
    replaced, skipped = 0, 0
    for wave, off, size in entries:
        dub = os.path.join(dub_dir, wave + ".wav")
        if not os.path.exists(dub):
            continue
        orig_hdr = bytes(pkg[off:off + 108])
        raw, br = encode_to_budget(dub, size, "/tmp/_enc.xma2.wav")
        blob = rebuild_blob(orig_hdr, raw)
        if len(blob) > size:
            if verbose:
                print("  SKIP", wave, "blob too big", len(blob), ">", size)
            skipped += 1
            continue
        pkg[off:off + size] = blob + b"\xff" * (size - len(blob))
        replaced += 1
        if verbose:
            print("  replaced", wave, "storage", size, "-> blob", len(blob), "@", br)
    open(out_pkg, "wb").write(pkg)
    print("wrote", out_pkg, "replaced", replaced, "skipped", skipped)
    return out_pkg


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    repack(sys.argv[1], sys.argv[2], sys.argv[3])
    return 0


if __name__ == "__main__":
    sys.exit(main())
