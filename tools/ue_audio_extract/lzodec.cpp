// lzodec.cpp — UE3/Xenon LZXP/LZO chunk decompressor (one-shot CLI tool).
//
// Reads one compressed chunk (or the whole stream of chunks in a package)
// from a CookedXenon *.xxx file and writes the decompressed logical bytes
// to stdout/file. The block decoder is the LZO1X implementation shipped in
// this project's own renderer/fonts path (`aot/src/cyrillic_fonts.cpp`),
// derived verbatim from the port codebase (same decoder used for the
// Russian fonts' page blobs). UE3 packages in this game are compressed with
// LZO chunks (CompressionFlags=2): UELib stops after the summary, so this
// tool is required before UELib can see the tables.
//
// Block layout inside a compressed chunk:
//   u32 Tag (UE signature, big-endian)
//   u32 ChunkSize (block uncompressed size, 0x20000)
//   u32 CompTotalBytes, UncompTotalBytes
//   blocks[]: pairs (compSize, uncompSize), big-endian
//   compressed data follows (each block is an independent LZO1X stream)
//
// usage: lzodec <in.xxx> <out.bin> [<HdrOff>]
//   HdrOff: offset of the chunk header; if omitted, the first UE signature
//   beyond the summary is used.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

static uint32_t U(const uint8_t* d, long off) {
  return ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) | ((uint32_t)d[off + 2] << 8) | d[off + 3];
}

// LZO1X block decoder (verbatim from aot/src/cyrillic_fonts.cpp).
static bool Lzo1xDecompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
  const uint8_t *ip = in, *ip_end = in + in_len;
  uint8_t *op = out, *op_end = out + out_len;
  size_t t = 0;
  const uint8_t *m_pos = nullptr;
  auto need_in = [&](size_t n) { return size_t(ip_end - ip) >= n; };
  auto need_out = [&](size_t n) { return size_t(op_end - op) >= n; };
  auto long_length = [&](size_t base) -> bool {
    while (need_in(1) && *ip == 0)
    {
      t += 255;
      ++ip;
    }
    if (!need_in(1))
      return false;
    t += base + *ip++;
    return true;
  };
  auto copy_literals = [&](size_t n) -> bool {
    if (!need_in(n) || !need_out(n))
      return false;
    std::memcpy(op, ip, n);
    op += n;
    ip += n;
    return true;
  };
  auto copy_match = [&](size_t n) -> bool {
    if (m_pos < out || m_pos >= op || !need_out(n))
      return false;
    for (size_t i = 0; i < n; ++i)
      *op++ = *m_pos++;
    return true;
  };

  if (!need_in(1))
    return false;
  bool skip_to_first = false, skip_to_next = false;
  if (*ip > 17)
  {
    t = *ip++ - 17;
    if (t < 4)
    {
      skip_to_next = true;
    }
    else
    {
      if (!copy_literals(t))
        return false;
      skip_to_first = true;
    }
  }
  for (;;)
  {
    if (!skip_to_first && !skip_to_next)
    {
      if (!need_in(1))
        return false;
      t = *ip++;
      if (t < 16)
      {
        if (t == 0 && !long_length(15))
          return false;
        if (!copy_literals(t + 3))
          return false;
        skip_to_first = true;
      }
    }
    if (skip_to_first)
    {
      skip_to_first = false;
      if (!need_in(1))
        return false;
      t = *ip++;
      if (t < 16)
      {
        if (!need_in(1))
          return false;
        m_pos = op - (1 + 0x0800) - (t >> 2) - (size_t(*ip++) << 2);
        if (!copy_match(3))
          return false;
        goto match_done;
      }
    }
    for (;;)
    {
      if (skip_to_next)
      {
        skip_to_next = false;
        goto match_next;
      }
      if (t >= 64)
      {
        if (!need_in(1))
          return false;
        m_pos = op - 1 - ((t >> 2) & 7) - (size_t(*ip++) << 3);
        t = (t >> 5) - 1;
        if (!copy_match(t + 2))
          return false;
      }
      else if (t >= 32)
      {
        t &= 31;
        if (t == 0 && !long_length(31))
          return false;
        if (!need_in(2))
          return false;
        m_pos = op - 1 - ((ip[0] >> 2) + (size_t(ip[1]) << 6));
        ip += 2;
        if (!copy_match(t + 2))
          return false;
      }
      else if (t >= 16)
      {
        m_pos = op - ((t & 8) << 11);
        t &= 7;
        if (t == 0 && !long_length(7))
          return false;
        if (!need_in(2))
          return false;
        m_pos -= (ip[0] >> 2) + (size_t(ip[1]) << 6);
        ip += 2;
        if (m_pos == op)
          return op == op_end;
        m_pos -= 0x4000;
        if (!copy_match(t + 2))
          return false;
      }
      else
      {
        if (!need_in(1))
          return false;
        m_pos = op - 1 - (t >> 2) - (size_t(*ip++) << 2);
        if (!copy_match(2))
          return false;
      }
    match_done:
      t = ip[-2] & 3;
      if (t == 0)
        break;
    match_next:
      if (!copy_literals(t))
        return false;
      if (!need_in(1))
        return false;
      t = *ip++;
    }
  }
}

int main(int argc, char** argv)
{
  if (argc < 3) {
    std::fprintf(stderr, "usage: lzodec <in.xxx> <out.bin> [<HdrOff>]\n");
    return 2;
  }
  const char* inpath = argv[1];
  const char* outpath = argv[2];
  long hdr = argc > 3 ? atol(argv[3]) : -1;

  FILE* fi = std::fopen(inpath, "rb");
  if (!fi) { std::perror("open"); return 1; }
  std::fseek(fi, 0, SEEK_END);
  long fsize = std::ftell(fi);
  std::fseek(fi, 0, SEEK_SET);
  std::vector<uint8_t> d(fsize);
  if (std::fread(d.data(), 1, fsize, fi) != (size_t)fsize) return 1;
  std::fclose(fi);

  if (hdr < 0) {
    hdr = -1;
    for (long i = 64; i + 4 <= fsize; ++i) {
      if (d[i] == 0x9e && d[i + 1] == 0x2a && d[i + 2] == 0x83 && d[i + 3] == 0xc1) { hdr = i; break; }
    }
    if (hdr < 0) { std::fprintf(stderr, "no chunk header\n"); return 1; }
  }
  std::fprintf(stderr, "hdr=%ld\n", hdr);
  uint32_t chunkSize = U(d.data(), hdr + 4);
  uint32_t total = U(d.data(), hdr + 12);
  std::fprintf(stderr, "chunkSize=%u totalUncomp=%u\n", chunkSize, total);

  std::vector<std::pair<uint32_t, uint32_t>> blocks;
  long hb = hdr + 16;
  for (uint32_t sum = 0; sum < total && blocks.size() < 256; hb += 8) {
    uint32_t cs = U(d.data(), hb), us = U(d.data(), hb + 4);
    blocks.emplace_back(cs, us);
    sum += us;
  }
  std::fprintf(stderr, "blocks=%zu\n", blocks.size());

  std::vector<uint8_t> out(total);
  long at = hb, written = 0;
  for (size_t b = 0; b < blocks.size(); ++b) {
    uint32_t cs = blocks[b].first, us = blocks[b].second;
    bool ok = Lzo1xDecompress(d.data() + at, cs, out.data() + written, us);
    std::fprintf(stderr, "block %zu comp=%u uncomp=%u ok=%d\n", b, cs, us, ok ? 1 : 0);
    if (!ok) return 1;
    at += cs;
    written += us;
  }
  FILE* fo = std::fopen(outpath, "wb");
  if (!fo) { std::perror("out"); return 1; }
  std::fwrite(out.data(), 1, out.size(), fo);
  std::fclose(fo);
  std::fprintf(stderr, "OK wrote %zu bytes\n", out.size());
  return 0;
}
