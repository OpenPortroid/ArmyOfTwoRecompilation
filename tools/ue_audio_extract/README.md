# UE3 audio extraction / re-encode / repack

Tools to pull the `SoundNodeWave` audio of Army of Two (Xbox 360, UE3 v445)
out of `assets/AO2Game/CookedXenon/*.xxx` packages, decode the XMA2 payload,
and splice replacement audio (e.g. a pt-BR dub) back into the same bulk
slots, so the game plays new text-to-speech in place of the original English
voice.

## Why this exists

- UE3 Xbox-360 packages here are **LZO chunk compressed** (`CompressionFlags=2`).
  UE Explorer / UELib returns early from `Deserialize` when compressed chunks
  exist ("We can't continue without decompressing"), so Names/Exports/Imports
  come back as 0 entries. That's UELib behavior, not an API mistake.
- The sound bulk element of each wave holds a **108-byte custom header**
  followed by the raw XMA2 payload (`CompressedXbox360Data`).

## Layout of the compressed package (445/79, big-endian)

```
[0:4]   Signature (0x9e2a83c1)
[4:8]   Version / Licensee
[8:12]  HeaderSize
...     table offsets: NameCount@101, ExportCount@Imports in ExportOffset, ...
[~89]   CompressionFlags = 2, chunk count (e.g. 7), entries:
        UncompressedOffset, CompressedOffset, UncompressedSize,
        CompressedSize (UELib ReadArray order; see ue_package.py)
Data (uncompressed logical start): offset 101 (name table)
```

**CompressedChunkHeader** at CompressedOffset: Tag=UE sig (4B), ChunkSize=0x20000
(4B), Summary (CompSize, UncompSize), N block pairs (compSize, uncompSize);
compressed payload starts right after (16 + 8*N bytes). Each block is an
independent LZO1X stream — decompression with the game's own decoder
(see `aot/src/cyrillic_fonts.cpp`; we include a verbatim copy in `lzodec.cpp`).

## Bulk element layout (SoundNodeWave audio, 56 + 4*N bytes of header)

```
u32BE headerLen      (44)
u32BE payloadOffset  (52)
u32BE dataSize       — the XMA2 stream length
u32BE magic          (0x04010000)
u32BE 384            (encoder delay/rounding constant)
u32BE numSamples(A)  (~16384/sample-ish; 129408 in the pilot)
u32BE ~24000         sample rate (UE3 Xbox VO 24 kHz mono)
u32BE 0x10D60000     constant
u32BE ~6593
u32BE 2048           packet size (fixed)
u32BE numSamples     (decodable samples)
u32BE numSamples-518
u32BE nPackets
u32BE 0x01000000     marker
nPackets * u32BE     cumulative samples per packet (seek table; last == numSamples)
```

Audio payload starts at `StorageOffset + 56 + 4*nPackets`, length `dataSize`,
multiple of 2048 (13 blocks for the Clyde pilot = 26624 bytes).
Sample rate is **24000 Hz mono** (validated by pitch, RMS and decode).

## XMA2 packets

2048-byte blocks: big-endian header dword = frames starting in the packet
(bits 26..31), first-frame bit offset (15 bits), metadata (3), skip count
(8). Unused tail of a block is 0xFF padding. FFmpeg 9.x (`-f wav` with
`WAVE_FORMAT_XMA2`, or vgmstream with a TXTH) decodes this, as does the
game's own XAudio2/XMA decoder (`sdk/src/audio/xma_context.cpp`).

## Components

| File | Purpose |
| --- | --- |
| `lzodec.cpp` | One-shot LZXP/LZO block decoder CLI (verbatim LZO1X from the port's fonts path). |
| `ExtractManual.cs` + `chunk_dump.cs` | C# UELib dumpers (need `Eliot.UELib.dll`). They parse the summary/chunk table and list (wave, off, size) from the deserialized properties rather than calling `UBulkData.LoadData` (which translates offsets through the compressed-chunk stream and yields wrong buffers). |
| `ue_package.py` | Pure-Python helpers: chunk Table via UELib, decompress via `lzodec`, assemble plain packages. |
| `ue_audio_extract.py` | CLI: `.xxx` → per-wave `.xma2` + TXTH stubs (vgmstream decodes them). |
| `xma2_reencode.py` | CLI: encode WAVs → xma2 packets (external `xma2enc_cli.exe`, birabittoh/xma2enc), rebuild the bulk header (dataSize, numSamples, packet count, seek table), splice at the wave's StorageOffset, verify the export still parses. |

## Rebuild notes

```
REM fullscreen build on Windows (MSVC)
cl lzodec.cpp /EHsc /O2 /Fe:lzodec.exe

REM .NET tools (csc.exe via the VS Build Tools toolchain)
csc.exe /nologo /out:chunk_dump.exe /r:Eliot.UELib.dll chunk_dump.cs
csc.exe /nologo /out:ExtractManual.exe /r:Eliot.UELib.dll ExtractManual.cs
```

`xma2enc_cli.exe` is built from https://github.com/birabittoh/xma2enc
(WMA-Pro bitstream + XMA2 packet layer). Repack bitrates are auto-tuned so
the re-encoded blob fits the original bulk StorageSize (the export record
and the package's byte budget don't change).

## Validated results

- Full game inventory: 140 `*_LOC_int.xxx` packages → 9753 `SoundNodeWave`
  exports → decoded to 9753 WAVs (RMS ~5-7k, voice-grade, 24000 Hz).
- 902 mission lines matched 1:1 nome→linha `*_sound_vo_main.int` (en+pt) and
  cloned in pt-BR (`language="Portuguese"`, voice clones from each line's
  English reference); pilot of 5 lines approved by the author (intonation
  preserved, Portuguese accent).
- Repack of the market package: boot into the militarybase with the repacked
  `01_map_market_sound_LOC_int.xxx`, log free of XMA/audio errors.
