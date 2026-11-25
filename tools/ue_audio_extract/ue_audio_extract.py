#!/usr/bin/env python3
"""ue_audio_extract.py — extract SoundNodeWave audio from one UE3 LOC package.

Pipeline:
  1. Decompress LZXP/LZO chunks (lzodec.exe, see lzodec.cpp) and assemble a
     plain UE3 package (see ../../tools/ue_audio_extract/ue_package.py).
  2. Enumerate audios with the bundled C# inspector (`ExtractManual.exe`):
     every USoundNodeWave export plus its BulkData (StorageOffset,
     StorageSize) in the assembled package.
  3. Parse each bulk element's 56+4*packets-byte header (dataSize,
     numSamples, packet count + seek table), split the raw XMA2 payload and
     write a vgmstream TXTH stub next to every wave so they decode with:
         vgmstream-cli -o wave.wav wave.xma2

Usage:
    python ue_audio_extract.py <in.xxx> <outdir>
    python ue_audio_extract.py <in.xxx> <outdir> --no-assemble

Build prerequisites (in tools/ue_audio_extract/):
    * ExtractManual.exe  — compile ExtractManual.cs against Eliot.UELib.dll
    * lzodec.exe         — compile lzodec.cpp with cl (MSVC) or g++
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ue_package  # noqa: E402

EXTRACTOR = os.path.join(HERE, "ExtractManual.exe")


def read_u32_be(buf, off):
    return struct.unpack(">I", buf[off:off + 4])[0]


def parse_wave_bulk(blob):
    if len(blob) < 56:
        raise ValueError("bulk element too small")
    md = {
        "data_size": read_u32_be(blob, 8),
        "rate_f": read_u32_be(blob, 24),
        "num_samples": read_u32_be(blob, 40),
        "num_samples_tight": read_u32_be(blob, 44),
        "n_packets": read_u32_be(blob, 48),
    }
    md["hdr_len"] = 56 + 4 * md["n_packets"]
    md["sample_table"] = [read_u32_be(blob, 56 + 4 * i) for i in range(md["n_packets"])]
    return md


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, outdir = sys.argv[1], sys.argv[2]
    no_assemble = "--no-assemble" in sys.argv
    os.makedirs(outdir, exist_ok=True)
    assembled = os.path.join(outdir, "assembled.xxx")
    if no_assemble:
        assembled = src
    else:
        ue_package.assemble_package(src, assembled)
    csv = os.path.join(outdir, "waves.tsv")
    subprocess.run([EXTRACTOR, assembled, outdir, csv], check=True)
    n = 0
    for line in open(csv, encoding="utf-8"):
        if not line.startswith("wave,"):
            parts = line.rstrip("\n").split(",")
            if len(parts) < 5:
                continue
            wave, bulk_file = parts[0], parts[4]
            blob = open(os.path.join(outdir, bulk_file), "rb").read()
            md = parse_wave_bulk(blob)
            payload = blob[md["hdr_len"]: md["hdr_len"] + md["data_size"]]
            xma_path = os.path.join(outdir, wave + ".xma2")
            open(xma_path, "wb").write(payload)
            open(xma_path + ".txth", "w").write(
                "codec = XMA2\ninterleave = 0x800\n"
                "sample_rate = 24000\nchannels = 1\n"
                "num_samples = %d\n" % md["num_samples"]
            )
            print("wave %s: %d samples in %d packets" % (wave, md["num_samples"], md["n_packets"]))
            n += 1
    print("extracted %d waves into %s" % (n, outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
