// identity.cpp - The player's identity (identity.h).

#include "identity.h"

#include "log_files.h"
#include "utils.h"

// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(ao2_identity_file, "", "Online",
                      "The player's key pair (identity.h): empty for settings\\identity.key. A second copy of the "
                      "game on one PC needs a file of its own to be another player.");
REXCVAR_DEFINE_STRING(ao2_generated_gamertag, "", "Online", "The launcher's fallback when net_gamertag is empty.");

namespace ao2::identity
{
  namespace
  {
    constexpr char kFileTag[] = "ao2-identity-p256";
    constexpr size_t kMaxCachedKeys = 512;

    std::mutex g_mutex;
    bool g_ready = false;
    BCRYPT_ALG_HANDLE g_alg = nullptr;
    BCRYPT_KEY_HANDLE g_key = nullptr;
    PublicKey g_public = {};
    std::string g_player_id, g_friend_code;
    uint64_t g_xuid = 0;
    std::map<PublicKey, BCRYPT_KEY_HANDLE> g_peer_keys;

    void Log(const char *fmt, ...)
    {
      static std::mutex log_mutex;
      std::lock_guard lock(log_mutex);
      static FILE *f = ao2::OpenLogFile("identity.log");
      if (!f)
      {
        return;
      }
      std::time_t now = std::time(nullptr);
      std::tm local = {};
      localtime_s(&local, &now);
      std::fprintf(f, "[%02d:%02d:%02d] ", local.tm_hour, local.tm_min, local.tm_sec);
      va_list args;
      va_start(args, fmt);
      std::vfprintf(f, fmt, args);
      va_end(args);
      std::fputc('\n', f);
      std::fflush(f);
    }

    std::string Hex(const uint8_t *p, size_t n)
    {
      static const char kDigits[] = "0123456789abcdef";
      std::string out;
      out.reserve(n * 2);
      for (size_t i = 0; i < n; ++i)
      {
        out += kDigits[p[i] >> 4];
        out += kDigits[p[i] & 15];
      }
      return out;
    }

    std::vector<uint8_t> Unhex(std::string_view text)
    {
      auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      std::vector<uint8_t> out;
      if (text.size() % 2)
      {
        return out;
      }
      for (size_t i = 0; i < text.size(); i += 2)
      {
        int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0)
        {
          return {};
        }
        out.push_back(uint8_t(hi << 4 | lo));
      }
      return out;
    }

    std::array<uint8_t, 32> Sha256(std::string_view data)
    {
      std::array<uint8_t, 32> out = {};
      BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                 ULONG(data.size()), out.data(), ULONG(out.size()));
      return out;
    }

    std::filesystem::path KeyFile()
    {
      std::string file = REXCVAR_GET(ao2_identity_file);
      return file.empty() ? utils::SettingsDir() / "identity.key" : std::filesystem::path(file);
    }

    // BCRYPT_ECCPRIVATE_BLOB: header, X, Y, d (32 bytes each).
    bool ImportPrivate(const std::vector<uint8_t> &blob)
    {
      if (blob.size() != sizeof(BCRYPT_ECCKEY_BLOB) + 96)
      {
        return false;
      }
      auto *head = reinterpret_cast<const BCRYPT_ECCKEY_BLOB *>(blob.data());
      if (head->dwMagic != BCRYPT_ECDSA_PRIVATE_P256_MAGIC || head->cbKey != 32)
      {
        return false;
      }
      if (BCryptImportKeyPair(g_alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &g_key, const_cast<PUCHAR>(blob.data()),
                              ULONG(blob.size()), 0) != 0)
      {
        g_key = nullptr;
        return false;
      }
      std::memcpy(g_public.data(), blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), kPublicKeyBytes);
      return true;
    }

    std::vector<uint8_t> ExportPrivate()
    {
      ULONG size = 0;
      if (BCryptExportKey(g_key, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &size, 0) != 0)
      {
        return {};
      }
      std::vector<uint8_t> blob(size);
      if (BCryptExportKey(g_key, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), size, &size, 0) != 0)
      {
        return {};
      }
      blob.resize(size);
      return blob;
    }

    bool Load(const std::filesystem::path &path)
    {
      std::ifstream in(path);
      std::string tag, hex;
      if (!(in >> tag >> hex) || tag != kFileTag)
      {
        return false;
      }
      return ImportPrivate(Unhex(hex));
    }

    bool MakeAndSave(const std::filesystem::path &path)
    {
      if (BCryptGenerateKeyPair(g_alg, &g_key, 256, 0) != 0 || BCryptFinalizeKeyPair(g_key, 0) != 0)
      {
        g_key = nullptr;
        return false;
      }
      std::vector<uint8_t> blob = ExportPrivate();
      if (blob.size() != sizeof(BCRYPT_ECCKEY_BLOB) + 96)
      {
        return false;
      }
      std::memcpy(g_public.data(), blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), kPublicKeyBytes);
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      std::filesystem::path tmp = path;
      tmp += ".tmp";
      {
        std::ofstream out(tmp, std::ios::trunc);
        out << kFileTag << ' ' << Hex(blob.data(), blob.size()) << '\n';
        if (!out)
        {
          return false;
        }
      }
      std::filesystem::rename(tmp, path, ec);
      SecureZeroMemory(blob.data(), blob.size());
      return !ec;
    }

    BCRYPT_KEY_HANDLE PeerKey(const PublicKey &key)
    {
      if (auto it = g_peer_keys.find(key); it != g_peer_keys.end())
      {
        return it->second;
      }
      if (g_peer_keys.size() >= kMaxCachedKeys)
      {
        for (auto &[k, h] : g_peer_keys)
        {
          BCryptDestroyKey(h);
        }
        g_peer_keys.clear();
      }
      std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + kPublicKeyBytes);
      auto *head = reinterpret_cast<BCRYPT_ECCKEY_BLOB *>(blob.data());
      head->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
      head->cbKey = 32;
      std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), key.data(), kPublicKeyBytes);
      BCRYPT_KEY_HANDLE h = nullptr;
      if (BCryptImportKeyPair(g_alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &h, blob.data(), ULONG(blob.size()), 0) != 0)
      {
        return nullptr; // not a point on the curve
      }
      g_peer_keys[key] = h;
      return h;
    }
  } // namespace

  bool Start()
  {
    std::lock_guard lock(g_mutex);
    if (g_ready)
    {
      return true;
    }
    if (BCryptOpenAlgorithmProvider(&g_alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
    {
      Log("no ECDSA P-256 provider: unsigned");
      return false;
    }
    const std::filesystem::path path = KeyFile();
    bool loaded = Load(path);
    if (!loaded)
    {
      std::error_code ec;
      if (std::filesystem::exists(path, ec))
      {
        // Kept, not overwritten: the player's identity is in it.
        std::filesystem::path bad = path;
        bad += ".bad";
        std::filesystem::rename(path, bad, ec);
        Log("%s is no key pair: moved to %s", path.string().c_str(), bad.string().c_str());
      }
      if (!MakeAndSave(path))
      {
        Log("cannot make or save a key pair at %s: unsigned", path.string().c_str());
        if (g_key)
        {
          BCryptDestroyKey(g_key);
          g_key = nullptr;
        }
        return false;
      }
    }
    g_player_id = PlayerIdOf(g_public);
    g_friend_code = FriendCodeOf(g_player_id);
    g_xuid = XuidOf(g_player_id);
    char xuid[24];
    std::snprintf(xuid, sizeof(xuid), "%016llX", (unsigned long long)g_xuid);
    rex::cvar::SetFlagByName("net_xuid", xuid);
    g_ready = true;
    Log("%s %s: player %s, friend code %s, XUID %s", loaded ? "loaded" : "made", path.string().c_str(),
        g_player_id.c_str(), g_friend_code.c_str(), xuid);
    return true;
  }

  bool Ready()
  {
    std::lock_guard lock(g_mutex);
    return g_ready;
  }

  const PublicKey &OwnPublicKey() { return g_public; }
  const std::string &OwnPlayerId() { return g_player_id; }
  const std::string &OwnFriendCode() { return g_friend_code; }
  uint64_t OwnXuid() { return g_xuid; }

  const std::string &DefaultGamertag()
  {
    static const std::string name = [] {
      uint64_t bits = (OwnXuid() & 0x0000FFFFFFFFFFF0ull) >> 4;
      if (!Ready() && BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&bits), sizeof(bits),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
      {
        bits = GetTickCount64();
      }
      char suffix[7];
      std::snprintf(suffix, sizeof(suffix), "%06llu", static_cast<unsigned long long>((bits >> 1) % 1000000ull));
      return std::string((bits & 1) ? "Salem" : "Rios") + suffix;
    }();
    return name;
  }

  Signature Sign(std::string_view data)
  {
    Signature sig = {};
    auto hash = Sha256(data);
    std::lock_guard lock(g_mutex);
    ULONG size = 0;
    if (!g_key || BCryptSignHash(g_key, nullptr, hash.data(), ULONG(hash.size()), sig.data(), ULONG(sig.size()),
                                 &size, 0) != 0)
    {
      sig = {};
    }
    return sig;
  }

  bool Verify(const PublicKey &key, std::string_view data, const Signature &sig)
  {
    auto hash = Sha256(data);
    std::lock_guard lock(g_mutex);
    if (!g_alg)
    {
      return false;
    }
    BCRYPT_KEY_HANDLE h = PeerKey(key);
    return h && BCryptVerifySignature(h, nullptr, hash.data(), ULONG(hash.size()),
                                      const_cast<PUCHAR>(sig.data()), ULONG(sig.size()), 0) == 0;
  }

  std::string PlayerIdOf(const PublicKey &key)
  {
    auto hash = Sha256(std::string_view(reinterpret_cast<const char *>(key.data()), key.size()));
    return Hex(hash.data(), 16);
  }

  std::string FriendCodeOf(const std::string &player_id)
  {
    static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::vector<uint8_t> id = Unhex(player_id);
    if (id.size() < 5)
    {
      return "";
    }
    uint64_t bits = 0;
    for (int i = 0; i < 5; ++i)
    {
      bits = bits << 8 | id[i];
    }
    std::string code;
    for (int i = 7; i >= 0; --i)
    {
      code += kAlphabet[(bits >> (5 * i)) & 31];
      if (i == 4)
      {
        code += '-';
      }
    }
    return code;
  }

  uint64_t XuidOf(const std::string &player_id)
  {
    std::vector<uint8_t> id = Unhex(player_id);
    if (id.size() < 8)
    {
      return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
      v = v << 8 | id[i];
    }
    // The runtime's online range; the low nibble is the local user (a
    // split-screen guest), the game refuses bits 0x00C0000000000000.
    return 0x0009000000000000ull | (v & 0x0000FFFFFFFFFFF0ull);
  }

  std::string NormalizeFriendCode(std::string_view text)
  {
    std::string code;
    for (char c : text)
    {
      if (c == '-' || c == ' ')
      {
        continue;
      }
      c = char(std::toupper(static_cast<unsigned char>(c)));
      if (c == 'O')
      {
        c = '0';
      }
      else if (c == 'I' || c == 'L')
      {
        c = '1';
      }
      if (std::string_view("0123456789ABCDEFGHJKMNPQRSTVWXYZ").find(c) == std::string_view::npos)
      {
        return "";
      }
      code += c;
    }
    if (code.size() != 8)
    {
      return "";
    }
    return code.substr(0, 4) + "-" + code.substr(4);
  }
} // namespace ao2::identity
