// cyrillic_fonts.cpp - Russian letters in the game's fonts.
//
// The Russian text (game_language.cpp) is CP1251 in the game's 8-bit
// localization: А-я are the bytes C0-FF, Ё ё are A8 and B8. The engine widens
// a byte to the same TCHAR, so Russian letters reach the fonts as the Latin-1
// characters À-ÿ, ¨ and ¸. Every font (UFont: Characters, 256 FFontCharacter
// rectangles in its pages; Textures, DXT5 pages) has those 66 characters as
// accented Latin letters; this moves them to Cyrillic letters drawn for the
// font.
//
// The fonts are read from the disc's packages (00_fonts, 00_shell's menu font,
// EngineFonts; uncompressed packages whose page data is LZO). The letters that
// are Latin ones (А В Е К М Н О Р С Т Х а е о р с у х, Ё ё as Ë ë) are the
// font's own pictures: their characters point at the Latin rectangles. The
// others are drawn in the font's look, taken from its own Latin letters: the
// cap height and baseline of H, the stem, the side bearings; the typeface the
// Windows one whose Latin comes out closest (Chosen, ao2_cyrillic_faces_report)
// sized, stretched and emboldened to that; the colors, and for fonts with an
// outline or a shadow the look learnt from the Latin letters (BuildLook). They
// are drawn as the font's texels are (4x, averaged, unsmoothed fonts cut) and
// go into the page room no kept character uses (ASCII, « » © ®): the control
// characters' boxes, the accented letters themselves, empty areas.
//
// The native renderer recognizes a page by its data (the disc's bytes, hashed)
// and samples our picture instead: the page at 2-4x, bilinear, with the
// letters' texels enlarged the same way. Once the pictures exist, the game
// thread rewrites the 66 characters of each loaded font (Tick).
#include "cyrillic_fonts.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include "gpu/native/texture_loader.h"
#include "ue3_reflect.h"
#include "utils.h"

REXCVAR_DEFINE_BOOL(ao2_cyrillic_faces_report, false, "Debug",
                    "Score every candidate typeface against each font's Latin letters (slow; the log lists the best)");
REXCVAR_DEFINE_BOOL(ao2_cyrillic_dump, false, "Debug",
                    "Write the font pages with Russian letters as BMP files (cyrillic_fonts in the user data folder)");

namespace ao2::cyrillic
{
  bool BuildPage(int page, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);

  namespace
  {
    namespace fs = std::filesystem;

    // ---------------------------------------------------------------------
    // Packages
    // ---------------------------------------------------------------------

    // LZO1X (the packages' page data).
    bool Lzo1xDecompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
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
              return op == op_end; // end of stream
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

    // A cooked UE3 package of the disc (big-endian, version 445). Only what
    // fonts need: names, imports, exports, tagged properties.
    struct Package
    {
      std::vector<uint8_t> d;
      std::vector<std::string> names;
      std::vector<std::string> import_names;
      struct Export
      {
        int32_t cls = 0, outer = 0;
        std::string name;
        uint32_t size = 0, offset = 0;
      };
      std::vector<Export> exports;

      uint32_t U(size_t o) const
      {
        if (o + 4 > d.size())
          return 0;
        uint32_t v;
        std::memcpy(&v, d.data() + o, 4);
        return std::byteswap(v);
      }
      int32_t I(size_t o) const { return int32_t(U(o)); }
      const std::string &Name(uint32_t index) const
      {
        static const std::string empty;
        return index < names.size() ? names[index] : empty;
      }

      bool Open(const fs::path &path)
      {
        std::ifstream in(path, std::ios::binary);
        if (!in)
          return false;
        d.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (U(0) != 0x9E2A83C1)
          return false;
        size_t o = 12;
        o += 4 + U(o); // folder name
        o += 4;        // package flags
        uint32_t name_count = U(o), name_offset = U(o + 4), export_count = U(o + 8), export_offset = U(o + 12),
                 import_count = U(o + 16), import_offset = U(o + 20);
        if (name_count > 1000000 || export_count > 1000000 || import_count > 1000000)
          return false;
        o = name_offset;
        for (uint32_t i = 0; i < name_count && o + 4 <= d.size(); ++i)
        {
          uint32_t n = U(o);
          o += 4;
          if (n == 0 || o + n > d.size())
            return false;
          names.emplace_back(reinterpret_cast<const char *>(d.data() + o), n - 1);
          o += n + 8; // the characters, then 64-bit flags
        }
        o = import_offset;
        for (uint32_t i = 0; i < import_count; ++i, o += 28)
          import_names.push_back(Name(U(o + 20)));
        o = export_offset;
        for (uint32_t i = 0; i < export_count; ++i)
        {
          Export e;
          e.cls = I(o);
          e.outer = I(o + 8);
          e.name = Name(U(o + 12));
          if (uint32_t number = U(o + 16))
            e.name += "_" + std::to_string(number - 1);
          e.size = U(o + 32);
          e.offset = U(o + 36);
          o += 40;
          o += 4 + 12 * size_t(U(o)); // component map
          o += 4;                     // export flags
          o += 4 + 4 * size_t(U(o));  // net objects
          o += 16;                    // guid
          if (o > d.size())
            return false;
          exports.push_back(std::move(e));
        }
        return true;
      }

      std::string ClassOf(const Export &e) const
      {
        if (e.cls < 0 && size_t(-e.cls - 1) < import_names.size())
          return import_names[size_t(-e.cls - 1)];
        return {};
      }

      struct Prop
      {
        std::string type;
        size_t offset = 0;
        uint32_t size = 0;
      };
      // The tagged properties at `o` (an object's start skips its net index).
      bool Props(size_t &o, std::map<std::string, Prop> &out) const
      {
        for (int guard = 0; guard < 256; ++guard)
        {
          if (o + 8 > d.size())
            return false;
          const std::string &name = Name(U(o));
          if (name == "None")
          {
            o += 8;
            return true;
          }
          Prop p;
          p.type = Name(U(o + 8));
          p.size = U(o + 16);
          o += 24;
          if (p.type == "StructProperty")
            o += 8;
          if (p.type == "BoolProperty")
            p.size = 4;
          p.offset = o;
          o += p.size;
          if (o > d.size())
            return false;
          out[name] = p;
        }
        return false;
      }
    };

    // ---------------------------------------------------------------------
    // Pictures
    // ---------------------------------------------------------------------

    struct Rgba
    {
      int w = 0, h = 0;
      std::vector<float> px; // straight alpha
      Rgba() = default;
      Rgba(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * h_ * 4, 0.0f) {}
      float *At(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
      const float *At(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
    };

    struct Gray
    {
      int w = 0, h = 0;
      std::vector<float> v;
      Gray() = default;
      Gray(int w_, int h_) : w(w_), h(h_), v(size_t(w_) * h_, 0.0f) {}
      float &At(int x, int y) { return v[size_t(y) * w + x]; }
      float At(int x, int y) const { return v[size_t(y) * w + x]; }
      float Get(int x, int y) const { return x < 0 || y < 0 || x >= w || y >= h ? 0.0f : At(x, y); }
    };

    // A DXT5 page (Xenos tiled, 16-bit big-endian words).
    Rgba DecodeDxt5(const std::vector<uint8_t> &raw, int w, int h)
    {
      Rgba out(w, h);
      int bw = w / 4, bh = h / 4;
      uint32_t pitch = (uint32_t(bw) + 31) & ~31u;
      for (int by = 0; by < bh; ++by)
      {
        for (int bx = 0; bx < bw; ++bx)
        {
          size_t at = size_t(ao2::gpu::TiledOffset2D(bx, by, pitch, 4));
          if (at + 16 > raw.size())
            continue;
          const uint8_t *src = raw.data() + at;
          uint8_t b[16];
          for (int i = 0; i < 16; i += 2)
          {
            b[i] = src[i + 1];
            b[i + 1] = src[i];
          }
          int a0 = b[0], a1 = b[1];
          int alpha[8] = {a0, a1};
          if (a0 > a1)
          {
            for (int i = 0; i < 6; ++i)
              alpha[2 + i] = ((6 - i) * a0 + (i + 1) * a1) / 7;
          }
          else
          {
            for (int i = 0; i < 4; ++i)
              alpha[2 + i] = ((4 - i) * a0 + (i + 1) * a1) / 5;
            alpha[6] = 0;
            alpha[7] = 255;
          }
          uint64_t abits = 0;
          for (int i = 0; i < 6; ++i)
            abits |= uint64_t(b[2 + i]) << (8 * i);
          uint16_t c0 = uint16_t(b[8] | (b[9] << 8)), c1 = uint16_t(b[10] | (b[11] << 8));
          uint32_t cbits = uint32_t(b[12]) | (uint32_t(b[13]) << 8) | (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);
          auto rgb = [](uint16_t v) {
            return std::array<float, 3>{float((v >> 11) & 31) / 31.0f, float((v >> 5) & 63) / 63.0f,
                                        float(v & 31) / 31.0f};
          };
          std::array<float, 3> p[4] = {rgb(c0), rgb(c1)};
          for (int k = 0; k < 3; ++k)
          {
            p[2][k] = (2 * p[0][k] + p[1][k]) / 3.0f;
            p[3][k] = (p[0][k] + 2 * p[1][k]) / 3.0f;
          }
          for (int i = 0; i < 16; ++i)
          {
            float *d = out.At(bx * 4 + i % 4, by * 4 + i / 4);
            const auto &col = p[(cbits >> (2 * i)) & 3];
            d[0] = col[0];
            d[1] = col[1];
            d[2] = col[2];
            d[3] = float(alpha[(abits >> (3 * i)) & 7]) / 255.0f;
          }
        }
      }
      return out;
    }

    // m shifted by (dx, dy): out(x + dx, y + dy) = m(x, y).
    Gray Shift(const Gray &m, int dx, int dy)
    {
      Gray out(m.w, m.h);
      for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x)
          out.At(x, y) = m.Get(x - dx, y - dy);
      return out;
    }

    // Maximum over a disc of radius r.
    Gray Dilate(const Gray &m, int r)
    {
      if (r <= 0)
        return m;
      Gray out = m;
      for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
          if (dx * dx + dy * dy <= r * r + r)
            for (int y = 0; y < m.h; ++y)
              for (int x = 0; x < m.w; ++x)
                out.At(x, y) = std::max(out.At(x, y), m.Get(x - dx, y - dy));
      return out;
    }

    // Area-weighted resampling of a coverage map to dw x dh.
    Gray Resample(const Gray &src, int dw, int dh)
    {
      Gray out(dw, dh);
      double fx = double(src.w) / dw, fy = double(src.h) / dh;
      std::vector<float> rows(size_t(src.w) * dh, 0.0f); // vertical pass first
      for (int y = 0; y < dh; ++y)
      {
        double y0 = y * fy, y1 = y0 + fy;
        for (int sy = int(y0); sy < src.h && sy < y1; ++sy)
        {
          double wgt = (std::min(y1, double(sy + 1)) - std::max(y0, double(sy))) / fy;
          if (wgt <= 0)
            continue;
          for (int x = 0; x < src.w; ++x)
            rows[size_t(y) * src.w + x] += float(src.At(x, sy) * wgt);
        }
      }
      for (int x = 0; x < dw; ++x)
      {
        double x0 = x * fx, x1 = x0 + fx;
        for (int sx = int(x0); sx < src.w && sx < x1; ++sx)
        {
          double wgt = (std::min(x1, double(sx + 1)) - std::max(x0, double(sx))) / fx;
          if (wgt <= 0)
            continue;
          for (int y = 0; y < dh; ++y)
            out.At(x, y) += float(rows[size_t(y) * src.w + sx] * wgt);
        }
      }
      for (float &v : out.v)
        v = std::clamp(v, 0.0f, 1.0f);
      return out;
    }

    // The learnt look's bins: a quarter texel each, from 3 texels inside a
    // letter's edge to 4 outside (beyond that nothing).
    constexpr int kLookStep = 4;
    constexpr float kLookIn = 3, kLookOut = 4;
    constexpr int kLookBins = int((kLookIn + kLookOut) * kLookStep);
    int LookBin(float d) { return std::clamp(int(std::floor((d + kLookIn) * kLookStep)), 0, kLookBins - 1); }
    int LookBand(float v, int bands) { return std::clamp(int(std::floor(v * bands)), 0, bands - 1); }
    size_t LookCell(int band, float d0, float d1)
    {
      return (size_t(band) * kLookBins + size_t(LookBin(d1))) * kLookBins + size_t(LookBin(d0));
    }

    // Bilinear enlargement by `k` (texel centers on pixel centers).
    Gray Enlarge(const Gray &src, int k)
    {
      Gray out(src.w * k, src.h * k);
      for (int y = 0; y < out.h; ++y)
      {
        float sy = (y + 0.5f) / k - 0.5f;
        int y0 = std::clamp(int(std::floor(sy)), 0, src.h - 1), y1 = std::min(y0 + 1, src.h - 1);
        float fy = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);
        for (int x = 0; x < out.w; ++x)
        {
          float sx = (x + 0.5f) / k - 0.5f;
          int x0 = std::clamp(int(std::floor(sx)), 0, src.w - 1), x1 = std::min(x0 + 1, src.w - 1);
          float fx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);
          out.At(x, y) = (src.At(x0, y0) * (1 - fx) + src.At(x1, y0) * fx) * (1 - fy) +
                         (src.At(x0, y1) * (1 - fx) + src.At(x1, y1) * fx) * fy;
        }
      }
      return out;
    }

    Gray Distance(const Gray &m);

    // Signed distance in texels (negative inside) from each pixel to the edge
    // of `cov` (coverage, `k` pixels per texel): to the half-coverage outline,
    // next to it the coverage itself.
    Gray SignedDistance(const Gray &cov, int k)
    {
      Gray inside(cov.w, cov.h), outside(cov.w, cov.h);
      for (size_t i = 0; i < cov.v.size(); ++i)
      {
        inside.v[i] = cov.v[i] > 0.5f ? 1.0f : 0.0f;
        outside.v[i] = 1.0f - inside.v[i];
      }
      Gray to_in = Distance(inside), to_out = Distance(outside), sd(cov.w, cov.h);
      for (size_t i = 0; i < cov.v.size(); ++i)
      {
        float d = inside.v[i] > 0.5f ? 0.5f - to_out.v[i] : to_in.v[i] - 0.5f;
        if (std::abs(d) < 1.0f)
          d = 0.5f - cov.v[i];
        sd.v[i] = d / k;
      }
      return sd;
    }

    // ---------------------------------------------------------------------
    // Letters (GDI)
    // ---------------------------------------------------------------------

    constexpr int kSuper = 4; // supersampling of the GDI rendering

    struct Raster
    {
      Gray cov;
      int baseline = 0; // row
    };

    // A typeface the letters are drawn with: a GDI family (a variable font's
    // named instances are families of their own) and weight.
    struct Face
    {
      const wchar_t *family;
      int weight;
    };

    class Gdi
    {
    public:
      Gdi() { dc_ = CreateCompatibleDC(nullptr); }
      ~Gdi()
      {
        for (auto &[k, f] : fonts_)
          DeleteObject(f);
        if (dc_)
          DeleteDC(dc_);
      }

      // Whether GDI has `family` (it substitutes another for a missing one)
      // with the Russian letters.
      bool Has(const wchar_t *family)
      {
        auto known = has_.find(family);
        if (known != has_.end())
          return known->second;
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, family);
        HGDIOBJ old = SelectObject(dc_, font);
        wchar_t name[LF_FACESIZE] = {};
        GetTextFaceW(dc_, LF_FACESIZE, name);
        WORD glyphs[4] = {};
        bool cyrillic = GetGlyphIndicesW(dc_, L"ЖЯжя", 4, glyphs, GGI_MARK_NONEXISTING_GLYPHS) == 4 &&
                        std::none_of(std::begin(glyphs), std::end(glyphs), [](WORD g) { return g == 0xFFFF; });
        SelectObject(dc_, old);
        DeleteObject(font);
        return has_[family] = _wcsicmp(name, family) == 0 && cyrillic;
      }

      // `ch` at an em of `em` pixels.
      Raster Render(wchar_t ch, double em, const Face &face)
      {
        Raster r;
        HFONT font = Font(face, std::max(1, int(std::lround(em))));
        HGDIOBJ old_font = SelectObject(dc_, font);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc_, &tm);
        SIZE size = {};
        GetTextExtentPoint32W(dc_, &ch, 1, &size);
        int pad = int(em / 2) + 4;
        int w = size.cx + 2 * pad, h = tm.tmHeight + 2 * pad;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp)
        {
          HGDIOBJ old_bmp = SelectObject(dc_, bmp);
          std::memset(bits, 0, size_t(w) * h * 4);
          SetBkMode(dc_, TRANSPARENT);
          SetTextColor(dc_, RGB(255, 255, 255));
          TextOutW(dc_, pad, pad, &ch, 1);
          GdiFlush();
          r.cov = Gray(w, h);
          const uint8_t *px = static_cast<const uint8_t *>(bits);
          for (size_t i = 0; i < r.cov.v.size(); ++i)
            r.cov.v[i] = float(std::max({px[i * 4], px[i * 4 + 1], px[i * 4 + 2]})) / 255.0f;
          r.baseline = pad + tm.tmAscent;
          SelectObject(dc_, old_bmp);
          DeleteObject(bmp);
        }
        SelectObject(dc_, old_font);
        return r;
      }

    private:
      // The letters of a face are drawn at a few sizes: their fonts are kept.
      HFONT Font(const Face &face, int px)
      {
        auto key = std::make_tuple(std::wstring(face.family), face.weight, px);
        auto it = fonts_.find(key);
        if (it != fonts_.end())
          return it->second;
        if (fonts_.size() >= 64)
        {
          for (auto &[k, f] : fonts_)
            DeleteObject(f);
          fonts_.clear();
        }
        HFONT font = CreateFontW(-px, 0, 0, 0, face.weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face.family);
        fonts_[key] = font;
        return font;
      }

      HDC dc_ = nullptr;
      std::map<std::tuple<std::wstring, int, int>, HFONT> fonts_;
      std::map<std::wstring, bool> has_;
    };

    struct Ink
    {
      int top = -1, bottom = -1, left = -1, right = -1; // inclusive
      bool empty() const { return top < 0; }
    };
    Ink InkOf(const Gray &m, float threshold)
    {
      Ink ink;
      for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x)
          if (m.At(x, y) > threshold)
          {
            if (ink.top < 0)
              ink.top = y;
            ink.bottom = y;
            ink.left = ink.left < 0 ? x : std::min(ink.left, x);
            ink.right = std::max(ink.right, x);
          }
      return ink;
    }

    // ---------------------------------------------------------------------
    // A font's look
    // ---------------------------------------------------------------------

    struct FontChar
    {
      int u = 0, v = 0, w = 0, h = 0, page = 0;
    };

    struct Style
    {
      bool effect = false;
      double dx = 0, dy = 0; // the effect's offset (texels)
      int r = 0;
      bool core_bright = true;
      bool binary = false; // the letters' alpha is 0 or 1 (no smoothing)
      float threshold = 0.5f;
      std::array<float, 3> core_rgb{1, 1, 1}, effect_rgb{0, 0, 0};
      // The effect's alpha by distance (texels) from the letter shifted by
      // (dx, dy): a drop shadow, an outline, a glow alike.
      std::vector<float> profile_d, profile_a;
      // The look learnt from the font's own letters (fonts with an effect):
      // premultiplied RGBA by the signed distance from the letter's edge, from
      // the edge of the letter moved by (sx, sy) quarter texels and by the
      // height in the line (bands from the baseline to the cap height).
      struct Look
      {
        int sx = 0, sy = 0, bands = 1;
        std::vector<std::array<float, 4>> cells;
      };
      std::shared_ptr<const Look> look;
      int cap_top = 0, base = 0;   // H's core rows (base: below the last)
      int ink_width = 0; // H's core
      double stem = 0;   // H's stem: the core's coverage across it
      int x_height = 0, n_width = 0;
      int cell_height = 0;
      int bear_upper[2] = {1, 1}, bear_lower[2] = {1, 1}; // left, right
      // The letters: typeface, em, horizontal stretch, emboldening (negative:
      // thinner), the lowercase's own scale (to x's height, n's width).
      Face face{L"Bahnschrift", 400};
      double em = 0, stretch = 1, embolden = 0, lower_x = 1, lower_y = 1;
    };

    struct SourceFont
    {
      std::string name;
      std::vector<FontChar> chars;
      std::vector<Rgba> pages;
      std::vector<std::vector<uint8_t>> raw; // the pages' disc bytes
      std::vector<int> page_w, page_h;
    };

    Rgba Cell(const SourceFont &f, int code)
    {
      const FontChar &c = f.chars[size_t(code)];
      Rgba out(std::max(c.w, 0), std::max(c.h, 0));
      if (c.page < 0 || size_t(c.page) >= f.pages.size())
        return out;
      const Rgba &p = f.pages[size_t(c.page)];
      for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
          if (c.u + x < p.w && c.v + y < p.h)
            std::memcpy(out.At(x, y), p.At(c.u + x, c.v + y), 16);
      return out;
    }

    float Luma(const float *p) { return (p[0] + p[1] + p[2]) / 3.0f; }

    Gray CoreMask(const Rgba &c, const Style &s)
    {
      Gray m(c.w, c.h);
      for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x)
        {
          const float *p = c.At(x, y);
          bool ink = p[3] > 0.3f;
          if (s.effect)
            ink = ink && ((Luma(p) > s.threshold) == s.core_bright);
          m.At(x, y) = ink ? 1.0f : 0.0f;
        }
      return m;
    }

    // How much of each pixel is the letter's core: its alpha, without the
    // effect's share (by luma between the effect's color and the core's).
    Gray CoreWeight(const Rgba &c, const Style &s)
    {
      Gray m(c.w, c.h);
      float lc = Luma(s.core_rgb.data()), le = Luma(s.effect_rgb.data());
      bool split = s.effect && std::abs(lc - le) > 0.1f;
      for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x)
        {
          const float *p = c.At(x, y);
          float t = split ? std::clamp((Luma(p) - le) / (lc - le), 0.0f, 1.0f) : 1.0f;
          if (s.effect && !split)
            t = (Luma(p) > s.threshold) == s.core_bright ? 1.0f : 0.0f;
          m.At(x, y) = p[3] * t;
        }
      return m;
    }

    Gray InkMask(const Rgba &c)
    {
      Gray m(c.w, c.h);
      for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x)
          m.At(x, y) = c.At(x, y)[3] > 0.3f ? 1.0f : 0.0f;
      return m;
    }

    // Euclidean distance (pixels) from every pixel to the nearest one of `m`
    // above 0.5 (0 on them); Felzenszwalb's separable transform.
    Gray Distance(const Gray &m)
    {
      constexpr double kFar = 1e12;
      int w = m.w, h = m.h, n = std::max(w, h);
      size_t len_max = static_cast<size_t>(n);
      std::vector<double> f(len_max), d(len_max), z(len_max + 1), grid(static_cast<size_t>(w) * h);
      std::vector<int> v(len_max);
      auto pass = [&](int len) {
        int k = 0;
        v[0] = 0;
        z[0] = -kFar;
        z[1] = kFar;
        for (int q = 1; q < len; ++q)
        {
          double s;
          for (;;)
          {
            s = ((f[size_t(q)] + double(q) * q) - (f[size_t(v[size_t(k)])] + double(v[size_t(k)]) * v[size_t(k)])) /
                (2.0 * q - 2.0 * v[size_t(k)]);
            if (s <= z[size_t(k)] && k > 0)
              --k;
            else
              break;
          }
          ++k;
          v[size_t(k)] = q;
          z[size_t(k)] = s;
          z[size_t(k) + 1] = kFar;
        }
        k = 0;
        for (int q = 0; q < len; ++q)
        {
          while (z[size_t(k) + 1] < q)
            ++k;
          double dq = q - v[size_t(k)];
          d[size_t(q)] = dq * dq + f[size_t(v[size_t(k)])];
        }
      };
      for (int x = 0; x < w; ++x)
      {
        for (int y = 0; y < h; ++y)
          f[size_t(y)] = m.At(x, y) > 0.5f ? 0.0 : kFar;
        pass(h);
        for (int y = 0; y < h; ++y)
          grid[size_t(y) * w + x] = d[size_t(y)];
      }
      Gray out(w, h);
      for (int y = 0; y < h; ++y)
      {
        for (int x = 0; x < w; ++x)
          f[size_t(x)] = grid[size_t(y) * w + x];
        pass(w);
        for (int x = 0; x < w; ++x)
          out.At(x, y) = float(std::sqrt(std::min(d[size_t(x)], 1e8)));
      }
      return out;
    }

    // The typefaces the letters may be drawn with: Windows' own sans serifs
    // with Cyrillic and Tektur (localization\fonts, loaded at Install). Each
    // font gets the one whose Latin letters come out closest to its own
    // (ChooseFace); those GDI does not have are skipped. Not Franklin Gothic:
    // its Л and Д are the Bulgarian ones.
    const std::vector<Face> &Candidates()
    {
      static const std::vector<Face> faces = {
          {L"Arial", 400},
          {L"Arial", FW_BOLD},
          {L"Arial Black", 900},
          {L"Arial Narrow", 400},
          {L"Arial Narrow", FW_BOLD},
          {L"Bahnschrift Light", 300},
          {L"Bahnschrift SemiLight", 350},
          {L"Bahnschrift", 400},
          {L"Bahnschrift SemiBold", 600},
          {L"Bahnschrift", FW_BOLD},
          {L"Bahnschrift Light SemiCondensed", 300},
          {L"Bahnschrift SemiLight SemiConde", 350},
          {L"Bahnschrift SemiCondensed", 400},
          {L"Bahnschrift SemiBold SemiConden", 600},
          {L"Bahnschrift Light Condensed", 300},
          {L"Bahnschrift SemiLight Condensed", 350},
          {L"Bahnschrift Condensed", 400},
          {L"Bahnschrift SemiBold Condensed", 600},
          {L"Calibri Light", 300},
          {L"Calibri", 400},
          {L"Calibri", FW_BOLD},
          {L"Candara", 400},
          {L"Candara", FW_BOLD},
          {L"Corbel", 400},
          {L"Corbel", FW_BOLD},
          {L"Century Gothic", 400},
          {L"Century Gothic", FW_BOLD},
          {L"Impact", 400},
          {L"Lucida Sans Unicode", 400},
          {L"Microsoft Sans Serif", 400},
          {L"MS Reference Sans Serif", 400},
          {L"Segoe UI Light", 300},
          {L"Segoe UI Semilight", 350},
          {L"Segoe UI", 400},
          {L"Segoe UI Semibold", 600},
          {L"Segoe UI", FW_BOLD},
          {L"Segoe UI Black", 900},
          {L"Tahoma", 400},
          {L"Tahoma", FW_BOLD},
          {L"Trebuchet MS", 400},
          {L"Trebuchet MS", FW_BOLD},
          {L"Verdana", 400},
          {L"Verdana", FW_BOLD},
          {L"Tektur", 400},
          {L"Tektur Medium", 500},
          {L"Tektur SemiBold", 600},
          {L"Tektur", FW_BOLD},
          {L"Tektur ExtraBold", 800},
          {L"Tektur Black", 900},
      };
      return faces;
    }

    bool Analyze(const SourceFont &f, Style &s, Gdi &gdi)
    {
      std::vector<Rgba> cells;
      for (char ch : std::string("HxognEM"))
        cells.push_back(Cell(f, ch));
      std::vector<float> lum;
      for (const Rgba &c : cells)
        for (int i = 0; i < c.w * c.h; ++i)
          if (c.px[size_t(i) * 4 + 3] > 0.3f)
            lum.push_back(Luma(&c.px[size_t(i) * 4]));
      if (lum.empty())
        return false;
      std::sort(lum.begin(), lum.end());
      float lo = lum[lum.size() * 5 / 100], hi = lum[lum.size() * 95 / 100];

      // The effect: the other color's pixels as the letter shifted and dilated.
      if (hi - lo > 0.15f)
      {
        float thr = (lo + hi) / 2, best = -1;
        for (int bright = 1; bright >= 0; --bright)
          for (int r = 0; r <= 2; ++r)
            for (int dy = -1; dy <= 3; ++dy)
              for (int dx = -1; dx <= 3; ++dx)
              {
                double inter = 0, uni = 0;
                for (const Rgba &c : cells)
                {
                  Gray core(c.w, c.h), eff(c.w, c.h);
                  for (int y = 0; y < c.h; ++y)
                    for (int x = 0; x < c.w; ++x)
                    {
                      const float *p = c.At(x, y);
                      bool ink = p[3] > 0.3f, is_core = ink && ((Luma(p) > thr) == bool(bright));
                      core.At(x, y) = is_core;
                      eff.At(x, y) = ink && !is_core;
                    }
                  Gray pred = Dilate(Shift(core, dx, dy), r);
                  for (size_t i = 0; i < pred.v.size(); ++i)
                  {
                    bool pp = pred.v[i] > 0.5f && core.v[i] < 0.5f, ee = eff.v[i] > 0.5f;
                    inter += pp && ee;
                    uni += pp || ee;
                  }
                }
                float score = float(inter / std::max(uni, 1.0)) - 0.01f * float(r + std::abs(dx) + std::abs(dy));
                if (score > best)
                {
                  best = score;
                  s.core_bright = bright;
                  s.dx = dx;
                  s.dy = dy;
                  s.r = r;
                  s.threshold = thr;
                }
              }
        s.effect = best > 0.35f;
      }
      // The core's color.
      std::array<double, 3> core{};
      double ncore = 0;
      for (const Rgba &c : cells)
      {
        Gray m = CoreMask(c, s);
        for (int y = 0; y < c.h; ++y)
          for (int x = 0; x < c.w; ++x)
            if (m.At(x, y) > 0.5f)
            {
              for (int k = 0; k < 3; ++k)
                core[k] += c.At(x, y)[k];
              ++ncore;
            }
      }
      if (ncore == 0)
        return false;
      for (int k = 0; k < 3; ++k)
        s.core_rgb[k] = float(core[k] / ncore);

      // The effect's alpha by distance from the shifted letter (half-texel
      // bins, all pixels outside the letter: the transparent ones count), its
      // color weighted by alpha.
      if (s.effect)
      {
        constexpr int kBins = 13;
        double sum[kBins] = {}, count[kBins] = {}, rgb[3] = {}, weight = 0;
        for (char ch : std::string("HxognEMoeasRDB"))
        {
          Rgba c = Cell(f, ch);
          Gray m = CoreMask(c, s);
          Gray d = Distance(Shift(m, int(s.dx), int(s.dy)));
          for (int y = 0; y < c.h; ++y)
            for (int x = 0; x < c.w; ++x)
            {
              if (m.At(x, y) > 0.5f)
                continue;
              const float *p = c.At(x, y);
              int bin = int(std::lround(d.At(x, y) * 2));
              if (bin < kBins)
              {
                sum[bin] += p[3];
                count[bin] += 1;
              }
              if (p[3] > 0.05f)
              {
                for (int k = 0; k < 3; ++k)
                  rgb[k] += p[k] * p[3];
                weight += p[3];
              }
            }
        }
        for (int b = 0; b < kBins; ++b)
          if (count[b] > 0)
          {
            s.profile_d.push_back(b * 0.5f);
            s.profile_a.push_back(float(sum[b] / count[b]));
          }
        for (int k = 0; k < 3; ++k)
          s.effect_rgb[k] = float(rgb[k] / std::max(weight, 1e-6));
        s.effect = !s.profile_d.empty();
      }

      // Unsmoothed letters: hardly any ink pixel between clear and opaque.
      {
        size_t ink = 0, part = 0;
        for (char ch : std::string("HOAEReaonsdkgx"))
        {
          Rgba c = Cell(f, ch);
          for (int i = 0; i < c.w * c.h; ++i)
          {
            float a = c.px[size_t(i) * 4 + 3];
            ink += a > 0.05f;
            part += a > 0.05f && a < 0.95f;
          }
        }
        s.binary = ink > 0 && part * 20 < ink;
      }

      // H: cap height, baseline, width, stem; x's height, n's width.
      Gray h = CoreWeight(cells[0], s);
      Ink ink = InkOf(h, 0.5f);
      if (ink.empty())
        return false;
      s.cap_top = ink.top;
      s.base = ink.bottom + 1;
      s.ink_width = ink.right - ink.left + 1;
      s.cell_height = f.chars['H'].h;
      int row = ink.top + (ink.bottom - ink.top) / 4;
      double run = 0;
      for (int x = ink.left; x < h.w && h.At(x, row) > 0.05f; ++x)
        run += h.At(x, row);
      for (int x = ink.left - 1; x >= 0 && h.At(x, row) > 0.05f; --x)
        run += h.At(x, row);
      s.stem = std::max(run, 0.5);
      Ink xi = InkOf(CoreWeight(Cell(f, 'x'), s), 0.5f), ni = InkOf(CoreWeight(Cell(f, 'n'), s), 0.5f);
      s.x_height = xi.empty() ? 0 : s.base - xi.top;
      s.n_width = ni.empty() ? 0 : ni.right - ni.left + 1;

      // Side bearings: medians over a few letters of each case.
      auto bearings = [&](const char *sample, int out[2]) {
        std::vector<int> l, r;
        for (const char *p = sample; *p; ++p)
        {
          Rgba c = Cell(f, *p);
          Ink k = InkOf(InkMask(c), 0.5f);
          if (k.empty())
            continue;
          l.push_back(k.left);
          r.push_back(c.w - 1 - k.right);
        }
        if (l.empty())
          return;
        std::sort(l.begin(), l.end());
        std::sort(r.begin(), r.end());
        out[0] = l[l.size() / 2];
        out[1] = r[r.size() / 2];
      };
      bearings("HNOEAD", s.bear_upper);
      bearings("noeadu", s.bear_lower);
      return true;
    }

    // `face`'s size, stretch and emboldening for H's look, the lowercase's
    // scale for x and n.
    bool FitFace(const Face &face, Style &s, Gdi &gdi)
    {
      int cap = s.base - s.cap_top;
      Raster probe = gdi.Render(L'H', cap * kSuper, face);
      Ink pk = InkOf(probe.cov, 0.5f);
      if (pk.empty())
        return false;
      double gcap = double(probe.baseline - pk.top) / kSuper;
      double em = cap * (cap / gcap);
      Raster r = gdi.Render(L'H', em * kSuper, face);
      Ink rk = InkOf(r.cov, 0.5f);
      if (rk.empty())
        return false;
      int prow = rk.top + (rk.bottom - rk.top) / 4, x0 = rk.left, x1 = x0;
      while (x1 < r.cov.w && r.cov.At(x1, prow) > 0.5f)
        ++x1;
      double gstem = double(x1 - x0) / kSuper, gwidth = double(rk.right - rk.left + 1) / kSuper;
      s.face = face;
      s.em = em;
      s.stretch = s.ink_width / gwidth;
      s.embolden = s.stem - gstem * s.stretch;
      s.lower_x = s.lower_y = 1;
      Raster x = gdi.Render(L'x', s.em * kSuper, s.face), n = gdi.Render(L'n', s.em * kSuper, s.face);
      Ink gx = InkOf(x.cov, 0.5f), gn = InkOf(n.cov, 0.5f);
      if (!gx.empty() && s.x_height > 0)
        s.lower_y = std::clamp(s.x_height / (double(x.baseline - gx.top) / kSuper), 0.8, 1.3);
      if (!gn.empty() && s.n_width > 0)
        s.lower_x = std::clamp(s.n_width / (double(gn.right - gn.left + 1) / kSuper * s.stretch), 0.8, 1.3);
      return true;
    }

    // ---------------------------------------------------------------------
    // The new letters and where they go
    // ---------------------------------------------------------------------

    // The 66 letters and their CP1251 bytes.
    struct Letter
    {
      wchar_t ch;
      int code;
    };
    std::vector<Letter> Letters()
    {
      std::vector<Letter> out;
      for (int i = 0; i < 64; ++i)
        out.push_back({wchar_t(0x410 + i), 0xC0 + i});
      out.push_back({L'Ё', 0xA8});
      out.push_back({L'ё', 0xB8});
      return out;
    }

    // Characters that keep their pictures: ASCII and the few Latin-1 signs the
    // Russian text uses.
    bool Kept(int code) { return (code >= 32 && code < 127) || code == 0xA9 || code == 0xAB || code == 0xAE || code == 0xBB; }

    struct Glyph
    {
      int code = 0;
      int w = 0;              // in page texels
      std::vector<uint8_t> rgba; // (w * scale) x (cell_height * scale), straight alpha
    };

    bool Upper(wchar_t ch) { return (ch >= L'A' && ch <= L'Z') || (ch >= 0x410 && ch < 0x430) || ch == L'Ё'; }

    // The letter's coverage at `scale` page pixels per texel and its baseline
    // row in it.
    bool LetterCore(Gdi &gdi, const Style &s, wchar_t ch, int scale, Gray &cov, int &baseline)
    {
      int super = scale >= 4 ? 2 : kSuper; // at 4x the texels are means of 16 pixels already
      Raster r = gdi.Render(ch, s.em * scale * super, s.face);
      Ink k = InkOf(r.cov, 0.1f);
      int base = s.base * scale;
      if (k.empty())
        return false;
      bool upper = Upper(ch);
      // To page pixels, stretched (the lowercase to the font's x and n).
      double kx = s.stretch * (upper ? 1.0 : s.lower_x), ky = upper ? 1.0 : s.lower_y;
      int dw = std::max(1, int(std::lround(r.cov.w * kx / super)));
      int dh = std::max(1, int(std::lround(r.cov.h * ky / super)));
      double sy = double(dh) / r.cov.h;
      baseline = int(std::lround(r.baseline * sy));
      int top = int(std::floor(k.top * sy));
      cov = Resample(r.cov, dw, dh);
      // Taller than the room above the baseline (Й in a tight cell): the mark
      // over the capital squeezed into the room left, the letter as it is;
      // without that room the whole letter squashed.
      int cap_row = baseline - (s.base - s.cap_top) * scale, room_top = baseline - base;
      if (baseline - top > base && baseline - top > 0)
      {
        if (upper && cap_row - room_top >= scale && top < cap_row)
        {
          Gray band(cov.w, cap_row - top);
          for (int y = 0; y < band.h; ++y)
            for (int x = 0; x < cov.w; ++x)
              band.At(x, y) = cov.At(x, top + y);
          Gray squeezed = Resample(band, cov.w, cap_row - room_top);
          for (int y = 0; y < cap_row; ++y)
            for (int x = 0; x < cov.w; ++x)
              cov.At(x, y) = y >= room_top ? squeezed.At(x, y - room_top) : 0.0f;
        }
        else
        {
          double squash = double(base) / (baseline - top);
          dh = std::max(1, int(std::lround(dh * squash)));
          baseline = int(std::lround(baseline * squash));
          cov = Resample(r.cov, dw, dh);
        }
      }
      // Emboldening: the letter smeared to the right; thinning: its left
      // edges eaten.
      double e = s.embolden * scale;
      for (double left = std::abs(e); std::abs(e) > 0.2 && left > 0; left -= 1)
      {
        float frac = float(std::min(1.0, left));
        Gray prev = cov;
        for (int y = 0; y < cov.h; ++y)
          for (int x = 0; x < cov.w; ++x)
          {
            float self = prev.At(x, y), left_of = x > 0 ? prev.At(x - 1, y) : (e > 0 ? 0.0f : 1.0f);
            cov.At(x, y) = e > 0 ? std::max(self, left_of * frac) : self - frac * (self - std::min(self, left_of));
          }
      }
      return true;
    }

    // The font's look for a letter's core (coverage at `scale` pixels per
    // texel, room for the effect around it, the baseline at row `baseline`):
    // the learnt look, else the core over the effect.
    Rgba Paint(const Style &s, const Gray &core, int baseline, int scale)
    {
      Gray effect(core.w, core.h);
      if (s.effect && !s.look)
      {
        Gray d = Distance(Shift(core, int(std::lround(s.dx * scale)), int(std::lround(s.dy * scale))));
        for (size_t i = 0; i < d.v.size(); ++i)
        {
          float dist = d.v[i] / scale, a = 0;
          const auto &xs = s.profile_d, &ys = s.profile_a;
          if (dist <= xs.front())
            a = ys.front();
          else if (dist <= xs.back())
          {
            size_t j = size_t(std::upper_bound(xs.begin(), xs.end(), dist) - xs.begin());
            float t = (dist - xs[j - 1]) / (xs[j] - xs[j - 1]);
            a = ys[j - 1] + (ys[j] - ys[j - 1]) * t;
          }
          effect.v[i] = a;
        }
      }
      Rgba look(core.w, core.h);
      if (s.look)
      {
        int cap = std::max(1, s.base - s.cap_top);
        Gray d0 = SignedDistance(core, scale);
        Gray d1 = SignedDistance(Shift(core, int(std::lround(s.look->sx * scale / 4.0)),
                                       int(std::lround(s.look->sy * scale / 4.0))),
                                 scale);
        for (int y = 0; y < core.h; ++y)
        {
          float v = (baseline - y - 0.5f) / (scale * cap);
          int band = s.look->bands > 1 ? LookBand(v, s.look->bands) : 0;
          for (int x = 0; x < core.w; ++x)
          {
            size_t i = size_t(y) * core.w + x;
            float *p = look.At(x, y);
            if (d0.v[i] >= kLookOut && d1.v[i] >= kLookOut)
              continue;
            const std::array<float, 4> &c = s.look->cells[LookCell(band, d0.v[i], d1.v[i])];
            p[3] = std::clamp(c[3], 0.0f, 1.0f);
            for (int ch = 0; ch < 3; ++ch)
              p[ch] = p[3] > 1e-4f ? std::clamp(c[size_t(ch)] / p[3], 0.0f, 1.0f) : 0.0f;
          }
        }
      }
      for (int y = 0; y < core.h && !s.look; ++y)
        for (int x = 0; x < core.w; ++x)
        {
          float ca = core.At(x, y), ea = effect.At(x, y);
          float a = ca + ea * (1 - ca);
          float *p = look.At(x, y);
          p[3] = a;
          for (int c = 0; c < 3; ++c)
            p[c] = a > 0 ? (s.core_rgb[c] * ca + s.effect_rgb[c] * ea * (1 - ca)) / a : 0;
        }

      return look;
    }

    // The letter at `scale` page pixels per texel, in its cell.
    Glyph DrawGlyph(Gdi &gdi, const Style &s, wchar_t ch, int code, int scale)
    {
      Glyph g;
      g.code = code;
      Gray cov;
      int baseline = 0;
      if (!LetterCore(gdi, s, ch, scale, cov, baseline))
        return g;
      int cell_h = s.cell_height * scale, base = s.base * scale;
      bool upper = Upper(ch);

      // The font's look.
      int pad = 4 * scale;
      Gray core(cov.w + 2 * pad, cov.h + 2 * pad);
      for (int y = 0; y < cov.h; ++y)
        for (int x = 0; x < cov.w; ++x)
          core.At(x + pad, y + pad) = cov.At(x, y);
      Rgba look = Paint(s, core, pad + baseline, scale);

      // The cell: the font's bearings around the ink, H's baseline.
      Gray ink_mask(look.w, look.h);
      for (size_t i = 0; i < ink_mask.v.size(); ++i)
        ink_mask.v[i] = look.px[i * 4 + 3];
      Ink ik = InkOf(ink_mask, 0.3f);
      if (ik.empty())
        return g;
      const int *bear = upper ? s.bear_upper : s.bear_lower;
      int x0 = ik.left - bear[0] * scale, x1 = ik.right + 1 + bear[1] * scale;
      // Whole texels, the nearest count (the ink centered in it).
      g.w = std::max(1, int(std::lround(double(x1 - x0) / scale)));
      x0 -= (g.w * scale - (x1 - x0)) / 2;
      int y0 = pad + baseline - base;
      int out_w = g.w * scale;
      g.rgba.assign(size_t(out_w) * cell_h * 4, 0);
      for (int y = 0; y < cell_h; ++y)
        for (int x = 0; x < out_w; ++x)
        {
          int lx = x0 + x, ly = y0 + y;
          if (lx < 0 || ly < 0 || lx >= look.w || ly >= look.h)
            continue;
          const float *p = look.At(lx, ly);
          uint8_t *o = &g.rgba[(size_t(y) * out_w + x) * 4];
          for (int c = 0; c < 4; ++c)
            o[c] = uint8_t(std::clamp(p[c], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
      return g;
    }

    // The letter as the game shows the font's own: a page texture of texels
    // (drawn at 4x, each texel the mean of its pixels; unsmoothed fonts'
    // alpha cut at half), enlarged to `scale` bilinear like the pages.
    Glyph MakeGlyph(Gdi &gdi, const Style &s, wchar_t ch, int code, int scale)
    {
      constexpr int k = 4;
      Glyph hi = DrawGlyph(gdi, s, ch, code, k), g;
      g.code = code;
      g.w = hi.w;
      if (hi.w <= 0)
        return g;
      int w = hi.w, h = s.cell_height;
      // The texels for the letter moved by (mx, my) pixels.
      auto texelize = [&](int mx, int my) {
        std::array<Gray, 4> t;
        for (Gray &p : t)
          p = Gray(w, h);
        for (int y = 0; y < h * k; ++y)
          for (int x = 0; x < w * k; ++x)
          {
            int tx = x + mx, ty = y + my;
            if (tx < 0 || ty < 0 || tx >= w * k || ty >= h * k)
              continue;
            const uint8_t *p = &hi.rgba[(size_t(y) * w * k + x) * 4];
            float a = p[3] / 255.0f;
            for (int c = 0; c < 4; ++c)
              t[size_t(c)].At(tx / k, ty / k) += (c < 3 ? p[c] / 255.0f * a : a) / (k * k);
          }
        return t;
      };
      // On the texel grid as the font's own letters are (their stems whole
      // texels): the move under a texel that leaves the crispest texels.
      std::array<Gray, 4> texels;
      double sharpest = -1;
      for (int my = -1; my <= 1; ++my)
        for (int mx = 1 - k / 2; mx <= k / 2; ++mx)
        {
          std::array<Gray, 4> t = texelize(mx, my);
          double sharp = 0;
          for (float a : t[3].v)
            sharp += double(a) * a;
          sharp -= 1e-3 * (std::abs(mx) + std::abs(my)); // the least move when even
          if (sharp > sharpest)
          {
            sharpest = sharp;
            texels = std::move(t);
          }
        }
      if (s.binary)
        for (size_t i = 0; i < texels[3].v.size(); ++i)
        {
          float a = texels[3].v[i], keep = a >= 0.5f ? 1.0f / std::max(a, 1e-6f) : 0.0f;
          for (int c = 0; c < 4; ++c)
            texels[size_t(c)].v[i] *= keep;
        }
      // The cell from the texels: the ink (as the font's bearings were taken)
      // and the bearings around it.
      {
        int left = -1, right = -1;
        for (int x = 0; x < w; ++x)
          for (int y = 0; y < h; ++y)
            if (texels[3].At(x, y) > 0.3f)
            {
              left = left < 0 ? x : left;
              right = x;
              break;
            }
        if (left >= 0)
        {
          const int *bear = Upper(ch) ? s.bear_upper : s.bear_lower;
          int nw = bear[0] + (right - left + 1) + bear[1], dx = bear[0] - left;
          std::array<Gray, 4> moved;
          for (int c = 0; c < 4; ++c)
          {
            moved[size_t(c)] = Gray(nw, h);
            for (int y = 0; y < h; ++y)
              for (int x = 0; x < w; ++x)
                if (x + dx >= 0 && x + dx < nw)
                  moved[size_t(c)].At(x + dx, y) = texels[size_t(c)].At(x, y);
          }
          texels = std::move(moved);
          w = g.w = nw;
        }
      }
      std::array<Gray, 4> big;
      for (int c = 0; c < 4; ++c)
        big[size_t(c)] = Enlarge(texels[size_t(c)], scale);
      g.rgba.assign(size_t(w) * scale * h * scale * 4, 0);
      for (size_t i = 0; i < big[3].v.size(); ++i)
      {
        float a = std::clamp(big[3].v[i], 0.0f, 1.0f);
        uint8_t *o = &g.rgba[i * 4];
        for (int c = 0; c < 3; ++c)
          o[c] = uint8_t(std::clamp(a > 1e-4f ? big[size_t(c)].v[i] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        o[3] = uint8_t(a * 255.0f + 0.5f);
      }
      return g;
    }

    // How close `s` draws the font's own Latin letters: the overlap of the
    // letters' cores (coverage intersection over union, ink centered, on the
    // baseline), averaged over `sample`.
    double Score(const SourceFont &f, const Style &s, Gdi &gdi, const char *sample)
    {
      constexpr int kScale = 3;
      double sum = 0;
      int n = 0;
      for (const char *p = sample; *p; ++p)
      {
        Gray weight = CoreWeight(Cell(f, *p), s);
        if (InkOf(weight, 0.5f).empty())
          continue;
        ++n;
        // The disc's letter at 3x: its coverage bilinear, the outline at half.
        Gray orig = Enlarge(weight, kScale);
        Ink ok = InkOf(orig, 0.5f);
        Gray cov;
        int baseline = 0;
        if (ok.empty() || !LetterCore(gdi, s, wchar_t(*p), kScale, cov, baseline))
          continue;
        Ink k = InkOf(cov, 0.5f);
        if (k.empty())
          continue;
        int cx = int(std::lround((ok.left + ok.right - k.left - k.right) / 2.0)), oy = s.base * kScale - baseline;
        double best = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -2; dx <= 2; ++dx)
          {
            int ox = cx + dx, sy = oy + dy;
            int inter = 0, uni = 0;
            for (int y = std::min(ok.top, k.top + sy); y <= std::max(ok.bottom, k.bottom + sy); ++y)
              for (int x = std::min(ok.left, k.left + ox); x <= std::max(ok.right, k.right + ox); ++x)
              {
                bool a = x >= 0 && y >= 0 && x < orig.w && y < orig.h && orig.At(x, y) > 0.5f;
                int qx = x - ox, qy = y - sy;
                bool b = qx >= 0 && qy >= 0 && qx < cov.w && qy < cov.h && cov.At(qx, qy) > 0.5f;
                inter += a && b;
                uni += a || b;
              }
            best = std::max(best, uni ? double(inter) / uni : 0.0);
          }
        sum += best;
      }
      return n ? sum / n : 0;
    }

    // The typefaces chosen for the fonts: ao2_cyrillic_faces_report's best,
    // checked by eye on the comparison dumps.
    // (Arial Narrow comes with Office: Windows' own faces follow it.)
    const std::vector<Face> *Chosen(const std::string &font)
    {
      static const std::map<std::string, std::vector<Face>> table = {
          {"franklin_ao2_large", {{L"Arial", 400}}}, // Helvetica
          {"franklin_ao2_small", {{L"Arial", 400}}},
          {"hud_action_font_a", {{L"Arial Narrow", 400}, {L"Arial", 400}}},
          {"hud_action_font_b", {{L"Bahnschrift SemiLight Condensed", 350}}},
          {"hud_action_font_z", {{L"Bahnschrift SemiBold SemiConden", 600}}},
          {"mp_large", {{L"Bahnschrift", 400}}},
          {"mp_med", {{L"Bahnschrift SemiLight SemiConde", 350}}},
          {"mp_small", {{L"Arial Narrow", 400}, {L"Tahoma", 400}}},
          {"menu_font_selected", {{L"Bahnschrift SemiLight", 350}}},
          {"United_Sans_Rg_Bd", {{L"Bahnschrift SemiCondensed", 400}}},
          {"United_Sans_Rg_Md", {{L"Arial Narrow", FW_BOLD}, {L"Arial", FW_BOLD}}},
          {"United_Sans_Semi_Ext_Hv_Sz_20", {{L"Arial Black", 900}}},
          {"United_Sans_Semi_Ext_Md_Sz_14", {{L"Arial", 400}}},
          {"Fonts_Menu_sz20", {{L"Arial", 400}}},
          {"LargeFont", {{L"Verdana", 400}}},
          {"MediumFont", {{L"Verdana", 400}}},
          {"SmallFont", {{L"Segoe UI", 400}}},
          {"TinyFont", {{L"Arial", 400}}},
      };
      auto it = table.find(font);
      return it == table.end() ? nullptr : &it->second;
    }

    // The typeface for the font: the chosen one, else (or with
    // ao2_cyrillic_faces_report) the candidate that draws the font's Latin
    // letters closest to its own.
    bool ChooseFace(const SourceFont &f, Style &s, Gdi &gdi, std::vector<Style> *best = nullptr)
    {
      bool report = REXCVAR_GET(ao2_cyrillic_faces_report);
      if (const std::vector<Face> *faces = Chosen(f.name); faces && !report)
        for (const Face &face : *faces)
          if (gdi.Has(face.family) && FitFace(face, s, gdi))
            return true;
      std::vector<std::pair<double, Style>> fits;
      for (const Face &face : Candidates())
      {
        Style t = s;
        if (!gdi.Has(face.family) || !FitFace(face, t, gdi))
        {
          if (report)
          {
            std::wstring family = face.family;
            REXLOG_INFO("cyrillic_fonts: no {} {}", std::string(family.begin(), family.end()), face.weight);
          }
          continue;
        }
        fits.push_back({Score(f, t, gdi, report ? "ABCDEFGHKLMNOPRSTUVXYZabcdeghkmnopqrstuvxyz" : "HOAEReaon"), t});
      }
      if (fits.empty())
        return false;
      std::stable_sort(fits.begin(), fits.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
      s = fits.front().second;
      for (size_t i = 0; best && i < fits.size() && i < 4; ++i)
        best->push_back(fits[i].second);
      for (size_t i = 4; best && i < fits.size(); ++i)
        if (std::wstring(fits[i].second.face.family).starts_with(L"Tektur"))
        {
          best->push_back(fits[i].second);
          break;
        }
      std::string list;
      for (size_t i = 0; i < fits.size() && i < (report ? 8u : 1u); ++i)
      {
        std::wstring family = fits[i].second.face.family;
        list += fmt::format("{}{} {} {:.3f}", i ? ", " : "", std::string(family.begin(), family.end()),
                            fits[i].second.face.weight, fits[i].first);
      }
      REXLOG_INFO("cyrillic_fonts: {}: {}", f.name, list);
      return true;
    }

    // The look of a font with an effect, learnt from its own letters: their
    // shapes at 4x (bilinear), the texels' colors over their pixels (the
    // letters drawn with it become texels the same way, MakeGlyph). Each
    // pixel's color goes by its distance from
    // the letter's edge and from the edge of the letter moved (a shadow, a
    // bevel's light), the move the one that explains the colors best; by the
    // height in the line too when the letters are shaded top to bottom.
    void BuildLook(const SourceFont &f, Style &s)
    {
      if (!s.effect)
        return;
      constexpr int k = 4, kBands = 4;
      int cap = std::max(1, s.base - s.cap_top);
      struct Sample
      {
        Gray body, d0;
        std::vector<float> t; // premultiplied RGBA
      };
      auto load = [&](const char *letters) {
        std::vector<Sample> out;
        for (const char *p = letters; *p; ++p)
        {
          Rgba c = Cell(f, *p);
          if (c.w <= 0 || c.h <= 0)
            continue;
          Gray body = CoreWeight(c, s);
          if (InkOf(body, 0.5f).empty())
            continue;
          Sample smp;
          smp.body = Enlarge(body, k);
          smp.d0 = SignedDistance(smp.body, k);
          std::array<Gray, 4> planes;
          for (int ch = 0; ch < 4; ++ch)
          {
            Gray g(c.w, c.h);
            for (size_t i = 0; i < g.v.size(); ++i)
              g.v[i] = ch < 3 ? c.px[i * 4 + ch] * c.px[i * 4 + 3] : c.px[i * 4 + 3];
            planes[size_t(ch)] = Resample(g, g.w * k, g.h * k); // each texel over its pixels
          }
          smp.t.resize(smp.body.v.size() * 4);
          for (size_t i = 0; i < smp.body.v.size(); ++i)
            for (int ch = 0; ch < 4; ++ch)
              smp.t[i * 4 + ch] = planes[size_t(ch)].v[i];
          out.push_back(std::move(smp));
        }
        return out;
      };
      // The sums of the colors in each cell for a move; the error of their
      // means (far pixels: nothing).
      struct Sums
      {
        std::vector<double> rgba, n;
        double err = 0;
      };
      auto accumulate = [&](const std::vector<Sample> &samples, int sx, int sy, int bands) {
        Sums sums;
        size_t cells = size_t(bands) * kLookBins * kLookBins;
        sums.rgba.assign(cells * 4, 0);
        sums.n.assign(cells, 0);
        double sq = 0, explained = 0;
        for (const Sample &smp : samples)
        {
          Gray d1 = SignedDistance(Shift(smp.body, sx, sy), k);
          for (int y = 0; y < smp.body.h; ++y)
          {
            float v = (s.base * k - y - 0.5f) / (k * cap);
            int band = bands > 1 ? LookBand(v, bands) : 0;
            for (int x = 0; x < smp.body.w; ++x)
            {
              size_t i = size_t(y) * smp.body.w + x;
              const float *t = &smp.t[i * 4];
              for (int ch = 0; ch < 4; ++ch)
                sq += double(t[ch]) * t[ch];
              if (smp.d0.v[i] >= kLookOut && d1.v[i] >= kLookOut)
                continue;
              size_t cell = LookCell(band, smp.d0.v[i], d1.v[i]);
              for (int ch = 0; ch < 4; ++ch)
                sums.rgba[cell * 4 + ch] += t[ch];
              sums.n[cell] += 1;
            }
          }
        }
        for (size_t c = 0; c < cells; ++c)
          if (sums.n[c] > 0)
            for (int ch = 0; ch < 4; ++ch)
              explained += sums.rgba[c * 4 + ch] * sums.rgba[c * 4 + ch] / sums.n[c];
        sums.err = sq - explained;
        return sums;
      };

      std::vector<Sample> few = load("HOAEReaonsdk");
      if (few.empty())
        return;
      // Whole texels near the first guess, then quarter texels: x, y, x.
      int bx = 0, by = 0;
      double best = 1e300;
      for (int ty = -1; ty <= 3; ++ty)
        for (int tx = -1; tx <= 3; ++tx)
          if (std::abs(tx - s.dx) + std::abs(ty - s.dy) <= 2)
          {
            double err = accumulate(few, tx * k, ty * k, 1).err;
            if (err < best)
            {
              best = err;
              bx = tx * k;
              by = ty * k;
            }
          }
      for (int axis : {0, 1, 0})
      {
        int x0 = bx, y0 = by;
        for (int d = -k + 1; d < k; ++d)
        {
          int sx = x0 + (axis == 0 ? d : 0), sy = y0 + (axis == 1 ? d : 0);
          double err = d ? accumulate(few, sx, sy, 1).err : best;
          if (err < best)
          {
            best = err;
            bx = sx;
            by = sy;
          }
        }
      }
      int bands = accumulate(few, bx, by, kBands).err < 0.85 * best ? kBands : 1;

      // The table from all the letters; cells no letter reaches take their
      // neighbors' colors (the bands' from the bands together).
      std::vector<Sample> all = load("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
      auto table = [&](int nb) {
        Sums sums = accumulate(all, bx, by, nb);
        std::vector<std::array<float, 4>> cells(sums.n.size());
        std::vector<char> known(sums.n.size(), 0);
        for (size_t c = 0; c < cells.size(); ++c)
          if (sums.n[c] > 0)
          {
            for (int ch = 0; ch < 4; ++ch)
              cells[c][size_t(ch)] = float(sums.rgba[c * 4 + ch] / sums.n[c]);
            known[c] = 1;
          }
        return std::make_pair(cells, known);
      };
      auto [merged, merged_known] = table(1);
      for (bool grew = true; grew;)
      {
        grew = false;
        std::vector<char> was = merged_known;
        for (int b1 = 0; b1 < kLookBins; ++b1)
          for (int b0 = 0; b0 < kLookBins; ++b0)
          {
            size_t c = size_t(b1) * kLookBins + b0;
            if (was[c])
              continue;
            std::array<float, 4> sum{};
            int n = 0;
            const int nx[4] = {b0 - 1, b0 + 1, b0, b0}, ny[4] = {b1, b1, b1 - 1, b1 + 1};
            for (int j = 0; j < 4; ++j)
              if (nx[j] >= 0 && ny[j] >= 0 && nx[j] < kLookBins && ny[j] < kLookBins &&
                  was[size_t(ny[j]) * kLookBins + nx[j]])
              {
                for (int ch = 0; ch < 4; ++ch)
                  sum[size_t(ch)] += merged[size_t(ny[j]) * kLookBins + nx[j]][size_t(ch)];
                ++n;
              }
            if (n)
            {
              for (int ch = 0; ch < 4; ++ch)
                merged[c][size_t(ch)] = sum[size_t(ch)] / n;
              merged_known[c] = 1;
              grew = true;
            }
          }
      }
      auto look = std::make_shared<Style::Look>();
      look->sx = bx;
      look->sy = by;
      look->bands = bands;
      if (bands == 1)
        look->cells = merged;
      else
      {
        auto [cells, known] = table(bands);
        for (size_t c = 0; c < cells.size(); ++c)
          if (!known[c])
            cells[c] = merged[c % merged.size()];
        look->cells = cells;
      }
      s.look = look;
    }

    struct Placed
    {
      int code = 0, u = 0, v = 0, w = 0, h = 0, page = 0;
      size_t glyph = 0; // kSame: the font's own picture of a Latin letter
    };
    constexpr size_t kSame = SIZE_MAX;

    // Russian letters that are Latin ones in every typeface.
    constexpr std::pair<int, int> kLookAlikes[] = {
        {0xC0, 'A'}, {0xC2, 'B'}, {0xC5, 'E'}, {0xCA, 'K'}, {0xCC, 'M'}, {0xCD, 'H'}, {0xCE, 'O'},
        {0xD0, 'P'}, {0xD1, 'C'}, {0xD2, 'T'}, {0xD5, 'X'}, {0xE0, 'a'}, {0xE5, 'e'}, {0xEE, 'o'},
        {0xF0, 'p'}, {0xF1, 'c'}, {0xF3, 'y'}, {0xF5, 'x'}, {0xA8, 0xCB}, {0xB8, 0xEB}, // Ё Ë, ё ë
    };

    // The page at `scale`: the disc's, bilinear, with the letters drawn on it.
    void ComposePage(const Rgba &src, const std::vector<Placed> &letters, const std::vector<Glyph> &glyphs, int scale,
                     std::vector<uint8_t> &rgba)
    {
      uint32_t width = uint32_t(src.w * scale), height = uint32_t(src.h * scale);
      rgba.assign(size_t(width) * height * 4, 0);
      // The page, bilinear (premultiplied while filtering).
      for (uint32_t y = 0; y < height; ++y)
      {
        float sy = (y + 0.5f) / scale - 0.5f;
        int y0 = std::clamp(int(std::floor(sy)), 0, src.h - 1), y1 = std::min(y0 + 1, src.h - 1);
        float fy = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);
        for (uint32_t x = 0; x < width; ++x)
        {
          float sx = (x + 0.5f) / scale - 0.5f;
          int x0 = std::clamp(int(std::floor(sx)), 0, src.w - 1), x1 = std::min(x0 + 1, src.w - 1);
          float fx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);
          float acc[4] = {};
          auto add = [&](int xx, int yy, float wgt) {
            const float *s = src.At(xx, yy);
            acc[0] += s[0] * s[3] * wgt;
            acc[1] += s[1] * s[3] * wgt;
            acc[2] += s[2] * s[3] * wgt;
            acc[3] += s[3] * wgt;
          };
          add(x0, y0, (1 - fx) * (1 - fy));
          add(x1, y0, fx * (1 - fy));
          add(x0, y1, (1 - fx) * fy);
          add(x1, y1, fx * fy);
          uint8_t *d = &rgba[(size_t(y) * width + x) * 4];
          for (int k = 0; k < 3; ++k)
            d[k] = uint8_t(std::clamp(acc[3] > 0 ? acc[k] / acc[3] : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
          d[3] = uint8_t(std::clamp(acc[3], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
      }
      // The letters, each on a cleared cell with a texel around it.
      for (const Placed &l : letters)
      {
        const Glyph &g = glyphs[l.glyph];
        int x0 = (l.u - 1) * scale, y0 = (l.v - 1) * scale, x1 = (l.u + l.w + 1) * scale, y1 = (l.v + l.h + 1) * scale;
        for (int y = std::max(0, y0); y < std::min(int(height), y1); ++y)
          std::memset(&rgba[(size_t(y) * width + std::max(0, x0)) * 4], 0,
                      size_t(std::min(int(width), x1) - std::max(0, x0)) * 4);
        int gw = g.w * scale, gh = l.h * scale;
        for (int y = 0; y < gh; ++y)
          std::memcpy(&rgba[(size_t(l.v * scale + y) * width + size_t(l.u) * scale) * 4],
                      &g.rgba[size_t(y) * gw * 4], size_t(gw) * 4);
      }
    }

    // ---------------------------------------------------------------------
    // State
    // ---------------------------------------------------------------------

    struct Page
    {
      uint32_t width = 0, height = 0, bytes = 0;
      uint64_t hash = 0;
      Rgba original; // decoded, 1x
      std::vector<Placed> letters;
      size_t font = 0; // index in State::fonts
    };

    struct BuiltFont
    {
      std::string name;
      FontChar h; // 'H' as on the disc, to recognize the font in memory
      std::vector<Placed> letters;
      std::vector<Glyph> glyphs;
    };

    struct State
    {
      int scale = 2;
      std::vector<BuiltFont> fonts;
      std::vector<Page> pages;
    };

    std::mutex g_mutex;
    std::condition_variable g_cv;
    bool g_started = false;
    std::atomic<bool> g_ready{false};
    State g_state;

    constexpr uint32_t kDxt45 = 20; // xenos::TextureFormat::k_DXT4_5
    constexpr uint32_t kCharStride = 20; // FFontCharacter: StartU, StartV, USize, VSize, TextureIndex (padded)

    int PageScale()
    {
      double render_scale = std::atof(rex::cvar::GetFlagByName("ao2_native_gpu_resolution_scale").c_str());
      return std::clamp(int(std::ceil(render_scale)), 2, 4);
    }

    // The fonts of one package with their pages' pictures and disc bytes.
    std::vector<SourceFont> ReadFonts(const fs::path &path)
    {
      std::vector<SourceFont> out;
      Package pkg;
      if (!pkg.Open(path))
      {
        REXLOG_WARN("cyrillic_fonts: cannot read {}", path.string());
        return out;
      }
      for (const Package::Export &e : pkg.exports)
      {
        if (pkg.ClassOf(e) != "Font" || e.name == "Xbox360_18pt" || e.name == "ps3buttons_18pt")
          continue;
        std::map<std::string, Package::Prop> props;
        size_t o = e.offset + 4;
        if (!pkg.Props(o, props) || !props.count("Characters") || !props.count("Textures"))
          continue;
        SourceFont f;
        f.name = e.name;
        const Package::Prop &chars = props["Characters"];
        size_t co = chars.offset;
        int32_t count = pkg.I(co);
        co += 4;
        if (count != 256)
          continue;
        bool ok = true;
        for (int i = 0; i < count && ok; ++i)
        {
          std::map<std::string, Package::Prop> c;
          ok = pkg.Props(co, c);
          FontChar fc;
          auto get = [&](const char *name) -> int {
            auto it = c.find(name);
            if (it == c.end())
              return 0;
            return it->second.size == 4 ? pkg.I(it->second.offset) : pkg.d[it->second.offset];
          };
          fc.u = get("StartU");
          fc.v = get("StartV");
          fc.w = get("USize");
          fc.h = get("VSize");
          fc.page = get("TextureIndex");
          f.chars.push_back(fc);
        }
        const Package::Prop &tex = props["Textures"];
        int32_t pages = pkg.I(tex.offset);
        for (int32_t j = 0; j < pages && ok; ++j)
        {
          int32_t index = pkg.I(tex.offset + 4 + 4 * size_t(j));
          if (index <= 0 || size_t(index) > pkg.exports.size())
          {
            ok = false;
            break;
          }
          const Package::Export &t = pkg.exports[size_t(index) - 1];
          std::map<std::string, Package::Prop> tp;
          size_t to = t.offset + 4;
          if (!pkg.Props(to, tp) || !tp.count("SizeX") || !tp.count("SizeY"))
          {
            ok = false;
            break;
          }
          int w = pkg.I(tp["SizeX"].offset), h = pkg.I(tp["SizeY"].offset);
          if (tp.count("Format") && pkg.d[tp["Format"].offset] != 7) // PF_DXT5
          {
            ok = false;
            break;
          }
          to += 16 + 4; // a bulk data header, the mip count
          uint32_t flags = pkg.U(to), bulk_size = pkg.U(to + 8), bulk_offset = pkg.U(to + 12);
          std::vector<uint8_t> raw;
          if (flags & 0x10)
          {
            // LZO chunks: tag, block size, total compressed / uncompressed, blocks.
            size_t c = bulk_offset;
            uint32_t total = pkg.U(c + 12);
            std::vector<std::pair<uint32_t, uint32_t>> blocks;
            size_t hb = c + 16;
            for (uint32_t sum = 0; sum < total && blocks.size() < 64; hb += 8)
            {
              blocks.push_back({pkg.U(hb), pkg.U(hb + 4)});
              sum += blocks.back().second;
            }
            raw.resize(total);
            size_t at = hb, written = 0;
            for (auto [cs, us] : blocks)
            {
              if (at + cs > pkg.d.size() || written + us > raw.size() ||
                  !Lzo1xDecompress(pkg.d.data() + at, cs, raw.data() + written, us))
              {
                ok = false;
                break;
              }
              at += cs;
              written += us;
            }
          }
          else if (flags == 0 && size_t(bulk_offset) + bulk_size <= pkg.d.size())
          {
            raw.assign(pkg.d.begin() + bulk_offset, pkg.d.begin() + bulk_offset + bulk_size);
          }
          else
          {
            ok = false;
          }
          if (!ok)
            break;
          f.pages.push_back(DecodeDxt5(raw, w, h));
          f.page_w.push_back(w);
          f.page_h.push_back(h);
          f.raw.push_back(std::move(raw));
        }
        if (ok && !f.pages.empty())
          out.push_back(std::move(f));
        else
          REXLOG_WARN("cyrillic_fonts: {} in {} not read", e.name, path.filename().string());
      }
      return out;
    }

    void WriteBmp(const fs::path &path, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h);

    // The font's Latin letters as on the disc over the same letters drawn with
    // its look (ao2_cyrillic_dump): how close the Russian ones come.
    void WriteComparison(const fs::path &path, const SourceFont &src, const Style &style, Gdi &gdi, int scale)
    {
      const char *sample = "ABCDEHKMOPRSTXZabcdeghkmnopstuxy";
      std::vector<Rgba> orig;
      std::vector<Glyph> ours;
      int width = 0, cell_h = style.cell_height * scale;
      for (const char *p = sample; *p; ++p)
      {
        orig.push_back(Cell(src, *p));
        ours.push_back(MakeGlyph(gdi, style, wchar_t(*p), *p, scale));
        width += std::max(orig.back().w, ours.back().w) * scale + scale;
      }
      if (width <= 0 || cell_h <= 0)
        return;
      uint32_t w = uint32_t(width), h = uint32_t(3 * cell_h + 2 * scale);
      std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
      int x = 0;
      for (size_t i = 0; i < orig.size(); ++i)
      {
        // The disc's letter as the game shows it: bilinear (premultiplied).
        const Rgba &c = orig[i];
        std::array<Gray, 4> planes;
        for (int k = 0; k < 4; ++k)
        {
          Gray g(c.w, c.h);
          for (size_t j = 0; j < g.v.size(); ++j)
            g.v[j] = k < 3 ? c.px[j * 4 + k] * c.px[j * 4 + 3] : c.px[j * 4 + 3];
          planes[size_t(k)] = Enlarge(g, scale);
        }
        for (int y = 0; y < c.h * scale && y < cell_h; ++y)
          for (int xx = 0; xx < c.w * scale; ++xx)
          {
            size_t j = size_t(y) * planes[3].w + xx;
            float a = planes[3].v[j];
            uint8_t *d = &rgba[(size_t(y) * w + x + xx) * 4];
            for (int k = 0; k < 3; ++k)
              d[k] = uint8_t(std::clamp(a > 0 ? planes[size_t(k)].v[j] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
            d[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
          }
        const Glyph &g = ours[i];
        int gw = g.w * scale;
        for (int y = 0; y < cell_h && gw > 0; ++y)
          std::memcpy(&rgba[(size_t(cell_h + scale + y) * w + x) * 4], &g.rgba[size_t(y) * gw * 4], size_t(gw) * 4);
        // The disc's letter painted from its own body: what the look can do.
        int pad = 4 * scale;
        Gray body = Enlarge(CoreWeight(c, style), scale), core(body.w + 2 * pad, body.h + 2 * pad);
        for (int y = 0; y < body.h; ++y)
          for (int xx = 0; xx < body.w; ++xx)
            core.At(xx + pad, y + pad) = body.At(xx, y);
        Rgba look = Paint(style, core, pad + style.base * scale, scale);
        for (int y = 0; y < body.h && y < cell_h; ++y)
          for (int xx = 0; xx < body.w; ++xx)
          {
            const float *p = look.At(xx + pad, y + pad);
            uint8_t *d = &rgba[(size_t(2 * (cell_h + scale) + y) * w + x + xx) * 4];
            for (int k = 0; k < 4; ++k)
              d[k] = uint8_t(std::clamp(p[k], 0.0f, 1.0f) * 255.0f + 0.5f);
          }
        x += std::max(c.w, g.w) * scale + scale;
      }
      WriteBmp(path, rgba, w, h);
    }

    // Russian and Latin text in the font as the game draws it: the characters'
    // rectangles on the pages (ao2_cyrillic_dump).
    void WriteSample(const fs::path &path, const SourceFont &src, const BuiltFont &font, int scale)
    {
      std::vector<std::vector<uint8_t>> pages(src.pages.size());
      for (size_t p = 0; p < src.pages.size(); ++p)
      {
        std::vector<Placed> on;
        for (const Placed &l : font.letters)
          if (size_t(l.page) == p && l.glyph != kSame)
            on.push_back(l);
        ComposePage(src.pages[p], on, font.glyphs, scale, pages[p]);
      }
      std::vector<FontChar> chars = src.chars;
      for (const Placed &l : font.letters)
        chars[size_t(l.code)] = {l.u, l.v, l.w, l.h, l.page};
      auto bytes = [](const wchar_t *text) {
        std::vector<int> out;
        for (const wchar_t *p = text; *p; ++p)
          out.push_back(*p >= 0x410 && *p < 0x450 ? 0xC0 + (*p - 0x410) : *p == L'Ё' ? 0xA8 : *p == L'ё' ? 0xB8 : int(*p) & 0xFF);
        return out;
      };
      std::vector<std::vector<int>> lines = {bytes(L"Съешь же ещё этих мягких французских булок, да выпей чаю."),
                                             bytes(L"ШИРОКАЯ ЭЛЕКТРИФИКАЦИЯ ЮЖНЫХ ГУБЕРНИЙ ДАСТ МОЩНЫЙ ТОЛЧОК ПОДЪЁМУ"),
                                             bytes(L"The quick brown fox jumps over the lazy dog. SUBTITLES 0123")};
      int line_h = src.chars['H'].h * scale, width = 0;
      for (const auto &line : lines)
      {
        int w = 0;
        for (int code : line)
          w += chars[size_t(code)].w * scale;
        width = std::max(width, w);
      }
      if (width <= 0 || line_h <= 0)
        return;
      uint32_t w = uint32_t(width), h = uint32_t(line_h * lines.size());
      std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
      for (size_t li = 0; li < lines.size(); ++li)
      {
        int x = 0;
        for (int code : lines[li])
        {
          const FontChar &c = chars[size_t(code)];
          if (c.page < 0 || size_t(c.page) >= pages.size())
            continue;
          int pw = src.page_w[size_t(c.page)] * scale;
          for (int y = 0; y < c.h * scale && y < line_h; ++y)
            for (int xx = 0; xx < c.w * scale; ++xx)
            {
              size_t from = (size_t(c.v * scale + y) * pw + size_t(c.u * scale + xx)) * 4;
              if (from + 4 > pages[size_t(c.page)].size())
                continue;
              std::memcpy(&rgba[((li * line_h + y) * w + x + xx) * 4], &pages[size_t(c.page)][from], 4);
            }
          x += c.w * scale;
        }
      }
      WriteBmp(path, rgba, w, h);
    }

    void BuildFont(const SourceFont &src, Gdi &gdi, int scale, State &state, const fs::path &dump_dir)
    {
      Style style;
      std::vector<Style> best;
      if (!Analyze(src, style, gdi) || !ChooseFace(src, style, gdi, &best))
      {
        REXLOG_WARN("cyrillic_fonts: {}: no look found", src.name);
        return;
      }
      BuildLook(src, style);
      if (!dump_dir.empty())
        WriteComparison(dump_dir / (src.name + "_latin.bmp"), src, style, gdi, scale);
      if (!dump_dir.empty() && REXCVAR_GET(ao2_cyrillic_faces_report))
        for (size_t i = 0; i < best.size(); ++i)
          WriteComparison(dump_dir / fmt::format("{}_latin_{}.bmp", src.name, i), src, best[i], gdi, scale);
      BuiltFont font;
      font.name = src.name;
      font.h = src.chars['H'];
      auto drawn = [&](int code) {
        const FontChar &c = src.chars[size_t(code)];
        return c.w > 0 && c.h > 0 && c.page >= 0 && size_t(c.page) < src.pages.size();
      };
      // The look-alikes are the font's own Latin letters (Ё, ё: Ë, ë, when the
      // font has them: as wide as E, e and taller).
      std::vector<int> same(256, -1);
      for (auto [code, latin] : kLookAlikes)
      {
        if (!drawn(latin))
          continue;
        if (latin > 0x7F)
        {
          int base = latin == 0xCB ? 'E' : 'e';
          if (!drawn(base))
            continue;
          Ink dotted = InkOf(InkMask(Cell(src, latin)), 0.5f), plain = InkOf(InkMask(Cell(src, base)), 0.5f);
          if (dotted.empty() || plain.empty() ||
              std::abs((dotted.right - dotted.left) - (plain.right - plain.left)) > 1 ||
              dotted.bottom != plain.bottom || dotted.top >= plain.top)
            continue;
        }
        same[size_t(code)] = latin;
      }
      for (const Letter &letter : Letters())
      {
        if (same[size_t(letter.code)] >= 0)
          continue;
        Glyph g = MakeGlyph(gdi, style, letter.ch, letter.code, scale);
        if (g.w > 0)
          font.glyphs.push_back(std::move(g));
      }

      // Room: everything but the kept characters (with a texel around them).
      std::vector<std::vector<uint8_t>> used(src.pages.size());
      for (size_t p = 0; p < src.pages.size(); ++p)
        used[p].assign(size_t(src.page_w[p]) * src.page_h[p], 0);
      auto mark = [&](int page, int u, int v, int w, int h) {
        int pw = src.page_w[size_t(page)], ph = src.page_h[size_t(page)];
        for (int y = std::max(0, v - 1); y < std::min(ph, v + h + 1); ++y)
          for (int x = std::max(0, u - 1); x < std::min(pw, u + w + 1); ++x)
            used[size_t(page)][size_t(y) * pw + x] = 1;
      };
      for (int code = 0; code < 256; ++code)
      {
        const FontChar &c = src.chars[size_t(code)];
        if (Kept(code) && c.w > 0 && c.page >= 0 && size_t(c.page) < src.pages.size())
          mark(c.page, c.u, c.v, c.w, c.h);
      }
      for (int code = 0; code < 256; ++code)
        if (same[size_t(code)] >= 0)
        {
          const FontChar &c = src.chars[size_t(same[size_t(code)])];
          mark(c.page, c.u, c.v, c.w, c.h);
          font.letters.push_back({code, c.u, c.v, c.w, c.h, c.page, kSame});
        }
      // Widest first, each at the first free place (pages in order, rows, columns).
      std::vector<size_t> order(font.glyphs.size());
      for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
      std::stable_sort(order.begin(), order.end(),
                       [&](size_t a, size_t b) { return font.glyphs[a].w > font.glyphs[b].w; });
      int h = style.cell_height;
      for (size_t gi : order)
      {
        const Glyph &g = font.glyphs[gi];
        bool placed = false;
        for (size_t p = 0; p < src.pages.size() && !placed; ++p)
        {
          int pw = src.page_w[p], ph = src.page_h[p];
          // Summed area of the used texels.
          std::vector<int> sat(size_t(pw + 1) * (ph + 1), 0);
          for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x)
              sat[size_t(y + 1) * (pw + 1) + x + 1] = used[p][size_t(y) * pw + x] + sat[size_t(y) * (pw + 1) + x + 1] +
                                                      sat[size_t(y + 1) * (pw + 1) + x] - sat[size_t(y) * (pw + 1) + x];
          auto busy = [&](int x, int y) {
            return sat[size_t(y + h) * (pw + 1) + x + g.w] - sat[size_t(y) * (pw + 1) + x + g.w] -
                   sat[size_t(y + h) * (pw + 1) + x] + sat[size_t(y) * (pw + 1) + x];
          };
          for (int y = 0; y + h <= ph && !placed; ++y)
            for (int x = 0; x + g.w <= pw && !placed; ++x)
              if (busy(x, y) == 0)
              {
                font.letters.push_back({g.code, x, y, g.w, h, int(p), gi});
                mark(int(p), x, y, g.w, h);
                placed = true;
              }
        }
        if (!placed)
          REXLOG_WARN("cyrillic_fonts: {}: no room for letter {:02X}", src.name, g.code);
      }

      if (!dump_dir.empty())
        WriteSample(dump_dir / (src.name + "_text.bmp"), src, font, scale);
      state.fonts.push_back(std::move(font));
      const BuiltFont &built = state.fonts.back();
      for (size_t p = 0; p < src.pages.size(); ++p)
      {
        Page page;
        for (const Placed &l : built.letters)
          if (size_t(l.page) == p && l.glyph != kSame)
            page.letters.push_back(l);
        if (page.letters.empty())
          continue;
        page.width = uint32_t(src.page_w[p]);
        page.height = uint32_t(src.page_h[p]);
        page.bytes = uint32_t(src.raw[p].size());
        page.hash = XXH3_64bits(src.raw[p].data(), src.raw[p].size());
        page.original = src.pages[p];
        page.font = state.fonts.size() - 1;
        state.pages.push_back(std::move(page));
      }
      std::wstring family = style.face.family;
      REXLOG_INFO("cyrillic_fonts: {}: {} letters ({} {} at {:.1f}px, stretch {:.2f}, {:+.1f}, lowercase {:.2f}x{:.2f}; "
                  "effect {})",
                  built.name, built.letters.size(), std::string(family.begin(), family.end()), style.face.weight,
                  style.em, style.stretch, style.embolden, style.lower_x, style.lower_y,
                  style.look ? fmt::format("learnt, moved {:.2f},{:.2f}, {} band(s)", style.look->sx / 4.0,
                                           style.look->sy / 4.0, style.look->bands)
                             : style.effect ? fmt::format("{},{} max {:.2f}", style.dx, style.dy,
                                                          *std::max_element(style.profile_a.begin(), style.profile_a.end()))
                                            : "none");
    }

    // A 32-bit top-down BMP of straight-alpha RGBA.
    void WriteBmp(const fs::path &path, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
    {
      BITMAPFILEHEADER fh = {};
      BITMAPINFOHEADER ih = {};
      ih.biSize = sizeof(ih);
      ih.biWidth = LONG(w);
      ih.biHeight = -LONG(h);
      ih.biPlanes = 1;
      ih.biBitCount = 32;
      fh.bfType = 0x4D42;
      fh.bfOffBits = sizeof(fh) + sizeof(ih);
      fh.bfSize = fh.bfOffBits + w * h * 4;
      std::vector<uint8_t> bgra(rgba.size());
      for (size_t i = 0; i + 3 < rgba.size(); i += 4)
      {
        bgra[i] = rgba[i + 2];
        bgra[i + 1] = rgba[i + 1];
        bgra[i + 2] = rgba[i];
        bgra[i + 3] = rgba[i + 3];
      }
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
      out.write(reinterpret_cast<const char *>(&ih), sizeof(ih));
      out.write(reinterpret_cast<const char *>(bgra.data()), std::streamsize(bgra.size()));
    }

    void Build(fs::path game_data_root, fs::path dump_dir)
    {
      State state;
      state.scale = PageScale();
      auto start = std::chrono::steady_clock::now();
      if (!dump_dir.empty())
      {
        std::error_code ec;
        fs::create_directories(dump_dir, ec);
      }
      fs::path cooked = game_data_root / "AO2Game" / "CookedXenon";
      // The fonts of a package at once, each on a thread of its own (GDI
      // with a DC each), then in order.
      unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
      for (const char *package : {"00_fonts.xxx", "00_shell.xxx", "EngineFonts.xxx"})
      {
        std::vector<SourceFont> fonts = ReadFonts(cooked / package);
        std::vector<State> built(fonts.size());
        std::atomic<size_t> next{0};
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < std::min<size_t>(threads, fonts.size()); ++t)
          workers.emplace_back([&] {
            Gdi gdi;
            for (size_t i; (i = next++) < fonts.size();)
              BuildFont(fonts[i], gdi, state.scale, built[i], dump_dir);
          });
        for (std::thread &w : workers)
          w.join();
        for (State &b : built)
        {
          size_t first = state.fonts.size();
          for (BuiltFont &font : b.fonts)
            state.fonts.push_back(std::move(font));
          for (Page &page : b.pages)
          {
            page.font += first;
            state.pages.push_back(std::move(page));
          }
        }
      }
      {
        std::lock_guard lock(g_mutex);
        g_state = std::move(state);
        g_ready = true;
      }
      g_cv.notify_all();
      REXLOG_INFO("cyrillic_fonts: {} fonts, {} pages with Russian letters in {} ms", g_state.fonts.size(),
                  g_state.pages.size(),
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
      if (!dump_dir.empty())
      {
        for (size_t i = 0; i < g_state.pages.size(); ++i)
        {
          std::vector<uint8_t> rgba;
          uint32_t w = 0, h = 0;
          if (BuildPage(int(i), rgba, w, h))
            WriteBmp(dump_dir / fmt::format("{}_{:02}.bmp", g_state.fonts[g_state.pages[i].font].name, i), rgba, w, h);
        }
        REXLOG_INFO("cyrillic_fonts: pages written to {}", dump_dir.string());
      }
    }

    void WaitReady()
    {
      std::unique_lock lock(g_mutex);
      g_cv.wait(lock, [] { return g_ready.load(); });
    }

    uint32_t Load32(const uint8_t *base, uint32_t addr)
    {
      uint32_t v;
      std::memcpy(&v, base + addr, 4);
      return std::byteswap(v);
    }

    void Store32(uint8_t *base, uint32_t addr, uint32_t value)
    {
      value = std::byteswap(value);
      std::memcpy(base + addr, &value, 4);
    }

    // The loaded fonts' characters: true once every font is ours.
    bool PatchFonts(uint8_t *base, int &patched_now)
    {
      uint32_t font_class = ao2::ue3::FindClass(base, "Font");
      int32_t characters = ao2::ue3::PropertyOffset(base, "Font", "Characters");
      if (!font_class || characters < 0)
        return false;
      size_t done = 0;
      for (uint32_t i = 0, n = ao2::ue3::ObjectCount(base); i < n; ++i)
      {
        uint32_t object = ao2::ue3::ObjectAt(base, i);
        if (!object || Load32(base, object + 52) != font_class)
          continue;
        std::string name = ao2::ue3::ObjectName(base, object);
        const BuiltFont *font = nullptr;
        for (const BuiltFont &f : g_state.fonts)
          if (f.name == name)
            font = &f;
        if (!font || font->letters.empty())
          continue;
        uint32_t data = Load32(base, object + uint32_t(characters));
        uint32_t count = Load32(base, object + uint32_t(characters) + 4);
        if (!data || count < 256)
          continue;
        uint32_t h = data + 'H' * kCharStride;
        if (Load32(base, h) != uint32_t(font->h.u) || Load32(base, h + 4) != uint32_t(font->h.v) ||
            Load32(base, h + 8) != uint32_t(font->h.w) || Load32(base, h + 12) != uint32_t(font->h.h))
          continue; // not the disc's font
        const Placed &first = font->letters.front();
        uint32_t probe = data + uint32_t(first.code) * kCharStride;
        if (Load32(base, probe) == uint32_t(first.u) && Load32(base, probe + 4) == uint32_t(first.v) &&
            Load32(base, probe + 8) == uint32_t(first.w))
        {
          ++done;
          continue;
        }
        for (const Placed &l : font->letters)
        {
          uint32_t entry = data + uint32_t(l.code) * kCharStride;
          Store32(base, entry, uint32_t(l.u));
          Store32(base, entry + 4, uint32_t(l.v));
          Store32(base, entry + 8, uint32_t(l.w));
          Store32(base, entry + 12, uint32_t(l.h));
          base[entry + 16] = uint8_t(l.page);
        }
        ++done;
        ++patched_now;
      }
      return done == g_state.fonts.size();
    }
  } // namespace

  void Install(const fs::path &game_data_root, const fs::path &user_data_root)
  {
    {
      std::lock_guard lock(g_mutex);
      if (g_started)
        return;
      g_started = true;
    }
    // Tektur for this process only (FR_PRIVATE): its named instances become
    // GDI families of their own (Tektur Medium, ...).
    fs::path tektur = utils::RepoRoot() / "localization" / "fonts" / "Tektur.ttf";
    if (AddFontResourceExW(tektur.wstring().c_str(), FR_PRIVATE, nullptr) <= 0)
      REXLOG_WARN("cyrillic_fonts: {} not loaded", tektur.string());
    fs::path dump = REXCVAR_GET(ao2_cyrillic_dump) ? user_data_root / "cyrillic_fonts" : fs::path();
    std::thread([root = game_data_root, dump] {
      SetThreadDescription(GetCurrentThread(), L"cyrillic_fonts");
      Build(root, dump);
    }).detach();
  }

  void Tick(uint8_t *base)
  {
    // Looked for every 60 frames until every font is ours, then every 600
    // (fonts of a package loaded again get their letters again).
    static uint32_t frame = 0;
    static bool all = false;
    if (!g_ready.load(std::memory_order_acquire) || g_state.fonts.empty())
      return;
    if (++frame % (all ? 600 : 60) != 0)
      return;
    int now = 0;
    all = PatchFonts(base, now);
    if (now)
      REXLOG_INFO("cyrillic_fonts: Russian letters in {} loaded fonts", now);
  }

  uint32_t PageBytes(uint32_t format, uint32_t width, uint32_t height)
  {
    if (format != kDxt45)
      return 0;
    {
      std::lock_guard lock(g_mutex);
      if (!g_started)
        return 0;
    }
    WaitReady();
    for (const Page &p : g_state.pages)
      if (p.width == width && p.height == height)
        return p.bytes;
    return 0;
  }

  int PageOf(uint32_t format, uint32_t width, uint32_t height, const uint8_t *guest_data, uint32_t bytes)
  {
    if (format != kDxt45 || !g_ready.load(std::memory_order_acquire) || !guest_data)
      return -1;
    uint64_t hash = XXH3_64bits(guest_data, bytes);
    for (size_t i = 0; i < g_state.pages.size(); ++i)
    {
      const Page &p = g_state.pages[i];
      if (p.width == width && p.height == height && p.bytes == bytes && p.hash == hash)
        return int(i);
    }
    return -1;
  }

  bool BuildPage(int index, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
  {
    if (!g_ready.load(std::memory_order_acquire) || index < 0 || size_t(index) >= g_state.pages.size())
      return false;
    const Page &page = g_state.pages[size_t(index)];
    ComposePage(page.original, page.letters, g_state.fonts[page.font].glyphs, g_state.scale, rgba);
    width = uint32_t(page.original.w * g_state.scale);
    height = uint32_t(page.original.h * g_state.scale);
    return true;
  }
} // namespace ao2::cyrillic
