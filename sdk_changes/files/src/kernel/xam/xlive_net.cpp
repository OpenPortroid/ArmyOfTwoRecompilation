/**
 ******************************************************************************
 * ReXGlue : Xbox LIVE / System Link emulation for peer-to-peer play           *
 ******************************************************************************
 * Based on the Xenia Canary netplay fork (AdrianCassar/xenia-canary),         *
 * released under the BSD license.                                             *
 ******************************************************************************
 */

#include "xlive_net.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <random>
#include <set>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <spdlog/sinks/basic_file_sink.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_STRING(net_bind_ip, "", "Network",
                      "Local IPv4 to use for online play (empty = auto-detect). Set this to your "
                      "VPN adapter address (Radmin/ZeroTier/Tailscale) when playing over a VPN.");
REXCVAR_DEFINE_STRING(net_host_ip, "", "Network",
                      "IPv4 address(es) of the host to look for sessions on, comma separated. "
                      "Leave empty to only search the local network.");
REXCVAR_DEFINE_STRING(net_gamertag, "", "Network",
                      "Gamertag shown to other players (empty = computer name).");
REXCVAR_DEFINE_UINT32(net_discovery_port, 36020, "Network",
                      "UDP port used for session discovery between players.");
REXCVAR_DEFINE_BOOL(net_lan_discovery, true, "Network",
                    "Broadcast session searches on the local network.");
REXCVAR_DECLARE(bool, log_to_files);
REXCVAR_DEFINE_BOOL(net_trace_kernel, false, "Network",
                    "Write all kernel (XAM/xboxkrnl) log messages at debug level to krnl.log.");
REXCVAR_DEFINE_UINT32(net_search_timeout_ms, 1200, "Network",
                      "How long a session search waits for hosts to answer.");

namespace rex::kernel::xam::live {

using rex::system::KernelState;

namespace {

constexpr uint32_t kMagic = 0x4C4F4132;  // "2AOL"
constexpr uint16_t kVersion = 1;

enum PacketType : uint8_t {
  kQuery = 1,
  kReply = 2,
  kWhois = 3,
  kIam = 4,
};

// ---------------------------------------------------------------------------
// Serialization helpers (host little-endian on all supported platforms)
// ---------------------------------------------------------------------------

struct Writer {
  std::vector<uint8_t> b;
  template <typename T>
  void put(T v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
  }
  void bytes(const void* p, size_t n) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    b.insert(b.end(), q, q + n);
  }
  void blob(const std::vector<uint8_t>& v) {
    put<uint16_t>(static_cast<uint16_t>(std::min<size_t>(v.size(), 0xFFFF)));
    bytes(v.data(), std::min<size_t>(v.size(), 0xFFFF));
  }
};

struct Reader {
  const uint8_t* p;
  size_t n;
  size_t o = 0;
  bool ok = true;
  template <typename T>
  T get() {
    T v{};
    if (o + sizeof(T) > n) {
      ok = false;
      return v;
    }
    std::memcpy(&v, p + o, sizeof(T));
    o += sizeof(T);
    return v;
  }
  void bytes(void* out, size_t len) {
    if (o + len > n) {
      ok = false;
      std::memset(out, 0, len);
      return;
    }
    std::memcpy(out, p + o, len);
    o += len;
  }
  std::vector<uint8_t> blob() {
    uint16_t len = get<uint16_t>();
    std::vector<uint8_t> v;
    if (!ok || o + len > n) {
      ok = false;
      return v;
    }
    v.assign(p + o, p + o + len);
    o += len;
    return v;
  }
};

void WriteSession(Writer& w, const AdvertisedSession& s) {
  w.put(s.session_id);
  w.bytes(&s.key, sizeof(s.key));
  w.put(s.flags);
  w.put(s.max_public);
  w.put(s.max_private);
  w.put(s.open_public);
  w.put(s.open_private);
  w.blob(s.qos_data);
  w.put<uint16_t>(static_cast<uint16_t>(s.properties.size()));
  for (const auto& p : s.properties) {
    w.put(p.id);
    w.put<uint8_t>(p.is_context ? 1 : 0);
    w.put(p.scalar);
    w.blob(p.blob);
  }
}

AdvertisedSession ReadSession(Reader& r) {
  AdvertisedSession s;
  s.session_id = r.get<uint64_t>();
  r.bytes(&s.key, sizeof(s.key));
  s.flags = r.get<uint32_t>();
  s.max_public = r.get<uint32_t>();
  s.max_private = r.get<uint32_t>();
  s.open_public = r.get<uint32_t>();
  s.open_private = r.get<uint32_t>();
  s.qos_data = r.blob();
  uint16_t count = r.get<uint16_t>();
  for (uint16_t i = 0; i < count && r.ok; i++) {
    PropertyValue p;
    p.id = r.get<uint32_t>();
    p.is_context = r.get<uint8_t>() != 0;
    p.scalar = r.get<uint64_t>();
    p.blob = r.blob();
    s.properties.push_back(std::move(p));
  }
  return s;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Peer {
  XNADDR addr;
  uint64_t xuid;
};

struct State {
  std::once_flag started;
  uint32_t local_ip = 0;  // network order
  uint32_t bind_ip = 0;   // network order, 0 = INADDR_ANY
  uint8_t mac[6] = {};
  uint64_t identity_hash = 0;
  std::string gamertag;

  std::mutex mutex;
  std::map<uint32_t, Peer> peers;
  std::map<uint32_t, std::map<uint32_t, PropertyValue>> user_props;  // user -> id -> value
  std::map<uint64_t, std::vector<uint8_t>> qos_listen;               // session -> data
  std::map<uint64_t, AdvertisedSession> known_sessions;              // search cache
  std::vector<XSession*> advertised;

  uint64_t responder_socket = uint64_t(-1);
};

State& state() {
  static State s;
  return s;
}

uint64_t Fnv1a(const std::string& s, uint64_t h = 0xCBF29CE484222325ull) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001B3ull;
  }
  return h;
}

uint32_t ParseIp(const std::string& text) {
  in_addr a = {};
  if (inet_pton(AF_INET, text.c_str(), &a) == 1) {
    return a.s_addr;
  }
  return 0;
}

std::string IpToString(uint32_t ip) {
  char buf[INET_ADDRSTRLEN] = {};
  in_addr a = {};
  a.s_addr = ip;
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

std::vector<uint32_t> HostIps() {
  std::vector<uint32_t> out;
  std::string list = REXCVAR_GET(net_host_ip);
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    std::string item = list.substr(start, end - start);
    item.erase(0, item.find_first_not_of(" \t"));
    item.erase(item.find_last_not_of(" \t") + 1);
    if (!item.empty()) {
      uint32_t ip = ParseIp(item);
      if (ip) {
        out.push_back(ip);
      } else {
        NetLog("WARNING: Invalid net_host_ip entry '{}'", item);
      }
    }
    start = end + 1;
  }
  return out;
}

void CloseSocket(uint64_t s) {
#if REX_PLATFORM_WIN32
  closesocket(static_cast<SOCKET>(s));
#else
  close(static_cast<int>(s));
#endif
}

uint32_t DetectLocalIp() {
  // Route lookup without sending anything.
  uint64_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == uint64_t(-1)) {
    return htonl(INADDR_LOOPBACK);
  }
  sockaddr_in target = {};
  target.sin_family = AF_INET;
  target.sin_port = htons(9);
  target.sin_addr.s_addr = htonl(0x0A0A0A0A);  // 10.10.10.10
  uint32_t ip = htonl(INADDR_LOOPBACK);
  if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
    sockaddr_in local = {};
    socklen_t len = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0 &&
        local.sin_addr.s_addr != 0) {
      ip = local.sin_addr.s_addr;
    }
  }
  CloseSocket(s);
  return ip;
}

uint64_t OpenUdp(uint32_t bind_ip, uint16_t port, bool broadcast) {
  uint64_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == uint64_t(-1)) {
    return s;
  }
  int one = 1;
  if (broadcast) {
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&one), sizeof(one));
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = bind_ip;
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseSocket(s);
    return uint64_t(-1);
  }
  return s;
}

int WaitReadable(uint64_t s, uint32_t timeout_ms) {
  fd_set fds;
  FD_ZERO(&fds);
#if REX_PLATFORM_WIN32
  FD_SET(static_cast<SOCKET>(s), &fds);
  int nfds = 0;
#else
  FD_SET(static_cast<int>(s), &fds);
  int nfds = static_cast<int>(s) + 1;
#endif
  timeval tv = {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return select(nfds, &fds, nullptr, nullptr, &tv);
}

void WriteHeader(Writer& w, PacketType type) {
  w.put(kMagic);
  w.put(kVersion);
  w.put<uint8_t>(type);
  w.put<uint32_t>(REX_KERNEL_STATE() ? REX_KERNEL_STATE()->title_id() : 0);
}

bool ReadHeader(Reader& r, PacketType* type) {
  if (r.get<uint32_t>() != kMagic) return false;
  if (r.get<uint16_t>() != kVersion) return false;
  *type = static_cast<PacketType>(r.get<uint8_t>());
  uint32_t title = r.get<uint32_t>();
  uint32_t ours = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->title_id() : 0;
  return r.ok && (title == ours || !title || !ours);
}

void SendPacket(uint64_t s, const Writer& w, uint32_t ip, uint16_t port) {
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = ip;
  sendto(s, reinterpret_cast<const char*>(w.b.data()), static_cast<int>(w.b.size()), 0,
         reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

// Rewrites an XNADDR's addresses to the IP we actually reached the peer at.
void PinAddress(XNADDR* addr, uint32_t ip) {
  addr->ina = ip;
  addr->inaOnline = ip;
}

void ResponderLoop() {
  auto& st = state();
  std::vector<uint8_t> buf(65536);
  while (true) {
    sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    int n = recvfrom(st.responder_socket, reinterpret_cast<char*>(buf.data()),
                     static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&from),
                     &from_len);
    if (n <= 0) {
      if (st.responder_socket == uint64_t(-1)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    Reader r{buf.data(), static_cast<size_t>(n)};
    PacketType type;
    if (!ReadHeader(r, &type)) {
      continue;
    }
    uint32_t src_ip = from.sin_addr.s_addr;
    uint16_t src_port = ntohs(from.sin_port);

    if (type == kQuery || type == kWhois) {
      uint32_t nonce = r.get<uint32_t>();
      XNADDR requester = {};
      r.bytes(&requester, sizeof(requester));
      uint64_t requester_xuid = r.get<uint64_t>();
      uint64_t filter_id = type == kQuery ? r.get<uint64_t>() : 0;
      if (!r.ok) continue;

      if (!IsLocalMac(requester.abEnet)) {
        PinAddress(&requester, src_ip);
        RememberPeer(src_ip, requester, requester_xuid);
      }

      Writer w;
      WriteHeader(w, type == kQuery ? kReply : kIam);
      w.put(nonce);
      XNADDR self = {};
      FillLocalXnAddr(&self);
      w.bytes(&self, sizeof(self));
      w.put(LocalXuid(0));

      if (type == kQuery) {
        std::vector<XSession*> hosted;
        {
          std::lock_guard<std::mutex> lock(st.mutex);
          for (auto* session : st.advertised) {
            session->Retain();
            hosted.push_back(session);
          }
        }
        std::vector<AdvertisedSession> sessions;
        for (auto* session : hosted) {
          sessions.push_back(session->Snapshot());
          session->Release();
        }
        std::vector<AdvertisedSession> filtered;
        for (auto& s : sessions) {
          if (!filter_id || s.session_id == filter_id) filtered.push_back(std::move(s));
        }
        w.put<uint16_t>(static_cast<uint16_t>(filtered.size()));
        for (const auto& s : filtered) {
          WriteSession(w, s);
        }
        if (!filtered.empty()) {
          NetLog("Answered session query from {} with {} session(s)",
                       IpToString(src_ip), filtered.size());
        }
      }
      SendPacket(st.responder_socket, w, src_ip, src_port);
    }
  }
}

void Start() {
  auto& st = state();
  if (REXCVAR_GET(net_trace_kernel) && REXCVAR_GET(log_to_files)) {
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("krnl.log", true);
    rex::AddSink(rex::log::krnl(), sink);
    rex::SetCategoryLevel(rex::log::krnl(), spdlog::level::debug);
    if (auto logger = rex::GetLogger(rex::log::krnl())) {
      logger->flush_on(spdlog::level::debug);
    }
  }
#if REX_PLATFORM_WIN32
  WSADATA wsa = {};
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

  std::string bind_text = REXCVAR_GET(net_bind_ip);
  st.bind_ip = bind_text.empty() ? 0 : ParseIp(bind_text);
  st.local_ip = st.bind_ip ? st.bind_ip : DetectLocalIp();

  char host[256] = {};
  gethostname(host, sizeof(host) - 1);
  st.gamertag = REXCVAR_GET(net_gamertag);
  if (st.gamertag.empty()) {
    st.gamertag = host[0] ? host : "Player";
  }
  if (st.gamertag.size() > 15) {
    st.gamertag.resize(15);
  }

  // Stable per machine/interface/gamertag, distinct for two instances bound
  // to different addresses on the same PC.
  st.identity_hash = Fnv1a(st.gamertag, Fnv1a(host, Fnv1a(IpToString(st.local_ip))));
  st.mac[0] = 0x02;  // locally administered
  for (int i = 1; i < 6; i++) {
    st.mac[i] = static_cast<uint8_t>(st.identity_hash >> (8 * i));
  }

  uint16_t port = static_cast<uint16_t>(REXCVAR_GET(net_discovery_port));
  st.responder_socket = OpenUdp(st.bind_ip, port, true);
  if (st.responder_socket == uint64_t(-1)) {
    NetLog("WARNING: Could not bind discovery port {} - this instance cannot host", port);
  } else {
    std::thread(ResponderLoop).detach();
  }

  // Note: must not call public accessors here (they re-enter EnsureStarted).
  NetLog("Online ready: ip={} bind={} gamertag='{}' identity={:016X} discovery={}",
         IpToString(st.local_ip), st.bind_ip ? IpToString(st.bind_ip) : "any", st.gamertag,
         st.identity_hash, port);
}

// Sends a query/whois to the given targets and collects replies.
template <typename OnReply>
void Exchange(PacketType type, uint64_t filter_id, const std::vector<uint32_t>& targets,
              bool broadcast, uint32_t timeout_ms, OnReply&& on_reply) {
  EnsureStarted();
  auto& st = state();
  uint64_t s = OpenUdp(st.bind_ip, 0, true);
  if (s == uint64_t(-1)) {
    return;
  }

  std::random_device rd;
  uint32_t nonce = rd();

  Writer w;
  WriteHeader(w, type);
  w.put(nonce);
  XNADDR self = {};
  FillLocalXnAddr(&self);
  w.bytes(&self, sizeof(self));
  w.put(LocalXuid(0));
  if (type == kQuery) {
    w.put(filter_id);
  }

  uint16_t port = static_cast<uint16_t>(REXCVAR_GET(net_discovery_port));
  auto send_all = [&]() {
    for (uint32_t ip : targets) {
      SendPacket(s, w, ip, port);
    }
    if (broadcast) {
      SendPacket(s, w, htonl(INADDR_BROADCAST), port);
    }
  };

  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::milliseconds(timeout_ms);
  auto next_send = start;
  std::vector<uint8_t> buf(65536);
  while (true) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    if (now >= next_send) {
      send_all();  // resend a few times to survive packet loss
      next_send = now + std::chrono::milliseconds(300);
    }
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::min(deadline, next_send) - now)
                    .count();
    if (WaitReadable(s, static_cast<uint32_t>(std::max<int64_t>(wait, 1))) <= 0) {
      continue;
    }
    sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    int n = recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                     reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0) continue;
    Reader r{buf.data(), static_cast<size_t>(n)};
    PacketType rtype;
    if (!ReadHeader(r, &rtype)) continue;
    if (rtype != (type == kQuery ? kReply : kIam)) continue;
    if (r.get<uint32_t>() != nonce) continue;
    XNADDR peer = {};
    r.bytes(&peer, sizeof(peer));
    uint64_t peer_xuid = r.get<uint64_t>();
    if (!r.ok || IsLocalMac(peer.abEnet)) continue;
    uint32_t src_ip = from.sin_addr.s_addr;
    PinAddress(&peer, src_ip);
    RememberPeer(src_ip, peer, peer_xuid);
    uint32_t rtt = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              start)
            .count());
    if (on_reply(r, peer, peer_xuid, rtt)) {
      break;
    }
  }
  CloseSocket(s);
}

uint32_t PropertySize(uint32_t id) {
  switch ((id >> 28) & 0xF) {
    case 0:  // context
    case 1:  // int32
    case 5:  // float
      return 4;
    case 2:  // int64
    case 3:  // double
    case 7:  // datetime
      return 8;
    default:
      return 0;  // variable
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Identity & addressing
// ---------------------------------------------------------------------------

void NetLogLine(const std::string& line) {
  static std::mutex log_mutex;
  static FILE* file = REXCVAR_GET(log_to_files) ? std::fopen("net.log", "w") : nullptr;
  // Local wall-clock time, to line up with the EA emulator and game logs.
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  std::time_t secs = static_cast<std::time_t>(ms / 1000);
  std::tm local = {};
#if REX_PLATFORM_WIN32
  localtime_s(&local, &secs);
#else
  localtime_r(&secs, &local);
#endif
  std::lock_guard<std::mutex> lock(log_mutex);
  if (file) {
    std::fprintf(file, "[%02d:%02d:%02d.%03lld] %s\n", local.tm_hour, local.tm_min, local.tm_sec,
                 static_cast<long long>(ms % 1000), line.c_str());
    std::fflush(file);
  }
  REXKRNL_INFO("[net] {}", line);
}

void EnsureStarted() {
  std::call_once(state().started, Start);
}

uint32_t LocalIp() {
  EnsureStarted();
  return state().local_ip;
}

uint32_t BindIp() {
  EnsureStarted();
  return state().bind_ip;
}

uint64_t LocalMachineId() {
  EnsureStarted();
  uint64_t id = 0xFA00000000000000ull;
  for (int i = 0; i < 6; i++) {
    id |= static_cast<uint64_t>(state().mac[i]) << (8 * (5 - i));
  }
  return id;
}

uint64_t LocalXuid(uint32_t user_index) {
  EnsureStarted();
  // Online XUIDs live in the 0x0009xxxxxxxxxxxx range.
  uint64_t base = 0x0009000000000000ull | (state().identity_hash & 0x0000FFFFFFFFFFF0ull);
  return base | (user_index & 0xF);
}

std::string LocalGamertag() {
  EnsureStarted();
  return state().gamertag;
}

void FillLocalXnAddr(XNADDR* addr) {
  EnsureStarted();
  auto& st = state();
  std::memset(addr, 0, sizeof(*addr));
  addr->ina = st.local_ip;
  addr->inaOnline = st.local_ip;
  addr->wPortOnline = 3074;
  std::memcpy(addr->abEnet, st.mac, 6);
  // SGADDR: ina, spi, xbox_id, platform
  std::memcpy(addr->abOnline, &st.local_ip, 4);
  uint64_t machine = LocalMachineId();
  for (int i = 0; i < 8; i++) {
    addr->abOnline[8 + i] = static_cast<uint8_t>(machine >> (8 * (7 - i)));
  }
  addr->abOnline[16] = 0;  // Xbox 360
}

bool IsLocalMac(const uint8_t mac[6]) {
  EnsureStarted();
  return std::memcmp(mac, state().mac, 6) == 0;
}

void RememberPeer(uint32_t ip, const XNADDR& addr, uint64_t xuid) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mutex);
  bool is_new = !st.peers.count(ip);
  st.peers[ip] = Peer{addr, xuid};
  if (is_new) {
    NetLog("Peer {} xuid={:016X}", IpToString(ip), xuid);
  }
}

bool LookupPeer(uint32_t ip, XNADDR* out_addr) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mutex);
  auto it = st.peers.find(ip);
  if (it == st.peers.end()) {
    return false;
  }
  *out_addr = it->second.addr;
  return true;
}

// ---------------------------------------------------------------------------
// Properties & QoS
// ---------------------------------------------------------------------------

void SetUserContext(uint32_t user_index, uint32_t id, uint32_t value) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mutex);
  PropertyValue p;
  p.id = id;
  p.scalar = value;
  p.is_context = true;
  st.user_props[user_index][id] = std::move(p);
}

void SetUserProperty(uint32_t user_index, uint32_t id, const uint8_t* guest_data, uint32_t size) {
  PropertyValue p;
  p.id = id;
  uint32_t fixed = PropertySize(id);
  if (fixed == 4) {
    uint32_t v;
    std::memcpy(&v, guest_data, 4);
    p.scalar = rex::byte_swap(v);
  } else if (fixed == 8) {
    uint64_t v;
    std::memcpy(&v, guest_data, 8);
    p.scalar = rex::byte_swap(v);
  } else {
    p.blob.assign(guest_data, guest_data + size);
  }
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mutex);
  st.user_props[user_index][id] = std::move(p);
}

std::vector<PropertyValue> GetUserProperties(uint32_t user_index) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mutex);
  std::vector<PropertyValue> out;
  auto it = st.user_props.find(user_index);
  if (it != st.user_props.end()) {
    for (const auto& [id, v] : it->second) out.push_back(v);
  }
  return out;
}

uint32_t GetUserContext(uint32_t user_index, uint32_t id, uint32_t default_value) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mutex);
  auto it = st.user_props.find(user_index);
  if (it == st.user_props.end()) return default_value;
  auto jt = it->second.find(id);
  return jt == it->second.end() ? default_value : static_cast<uint32_t>(jt->second.scalar);
}

void SetQosListenData(uint64_t session_id, const uint8_t* data, uint32_t size, bool enable) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mutex);
  if (!enable) {
    st.qos_listen.erase(session_id);
    return;
  }
  if (data) {
    st.qos_listen[session_id].assign(data, data + size);
  } else {
    st.qos_listen.try_emplace(session_id);
  }
}

std::optional<AdvertisedSession> FindKnownSession(uint64_t session_id) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mutex);
  auto it = st.known_sessions.find(session_id);
  if (it == st.known_sessions.end()) return std::nullopt;
  return it->second;
}

void AdvertiseSession(XSession* session) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mutex);
  if (std::find(st.advertised.begin(), st.advertised.end(), session) == st.advertised.end()) {
    session->Retain();
    st.advertised.push_back(session);
    NetLog("Hosting session {:016X}", session->session_id());
  }
}

void WithdrawSession(XSession* session) {
  auto& st = state();
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    auto it = std::find(st.advertised.begin(), st.advertised.end(), session);
    if (it != st.advertised.end()) {
      st.advertised.erase(it);
      found = true;
    }
  }
  if (found) {
    NetLog("Stopped hosting session {:016X}", session->session_id());
    session->Release();
  }
}

std::vector<AdvertisedSession> SearchSessions(uint32_t timeout_ms) {
  std::vector<AdvertisedSession> results;
  std::set<uint64_t> seen;
  auto targets = HostIps();
  bool broadcast = REXCVAR_GET(net_lan_discovery);
  NetLog("Searching for sessions ({} direct host(s), LAN broadcast {})",
               targets.size(), broadcast ? "on" : "off");

  Exchange(kQuery, 0, targets, broadcast, timeout_ms,
           [&](Reader& r, const XNADDR& peer, uint64_t peer_xuid, uint32_t rtt) {
             uint16_t count = r.get<uint16_t>();
             for (uint16_t i = 0; i < count && r.ok; i++) {
               AdvertisedSession s = ReadSession(r);
               if (!r.ok || seen.count(s.session_id)) continue;
               s.host = peer;
               s.host_xuid = peer_xuid;
               s.rtt_ms = std::max<uint32_t>(rtt, 1);
               seen.insert(s.session_id);
               results.push_back(s);
             }
             return false;
           });

  auto& st = state();
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    for (const auto& s : results) st.known_sessions[s.session_id] = s;
  }
  NetLog("Search found {} session(s)", results.size());
  return results;
}

std::optional<AdvertisedSession> QuerySessionById(uint64_t session_id, uint32_t timeout_ms) {
  if (auto known = FindKnownSession(session_id)) {
    // Refresh from the host it came from.
    std::optional<AdvertisedSession> fresh;
    Exchange(kQuery, session_id, {known->host.ina}, false, timeout_ms,
             [&](Reader& r, const XNADDR& peer, uint64_t peer_xuid, uint32_t rtt) {
               uint16_t count = r.get<uint16_t>();
               for (uint16_t i = 0; i < count && r.ok; i++) {
                 AdvertisedSession s = ReadSession(r);
                 if (r.ok && s.session_id == session_id) {
                   s.host = peer;
                   s.host_xuid = peer_xuid;
                   s.rtt_ms = std::max<uint32_t>(rtt, 1);
                   fresh = s;
                   return true;
                 }
               }
               return false;
             });
    return fresh ? fresh : known;
  }

  std::optional<AdvertisedSession> found;
  Exchange(kQuery, session_id, HostIps(), REXCVAR_GET(net_lan_discovery), timeout_ms,
           [&](Reader& r, const XNADDR& peer, uint64_t peer_xuid, uint32_t rtt) {
             uint16_t count = r.get<uint16_t>();
             for (uint16_t i = 0; i < count && r.ok; i++) {
               AdvertisedSession s = ReadSession(r);
               if (r.ok && s.session_id == session_id) {
                 s.host = peer;
                 s.host_xuid = peer_xuid;
                 s.rtt_ms = std::max<uint32_t>(rtt, 1);
                 found = s;
                 return true;
               }
             }
             return false;
           });
  if (found) {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().known_sessions[session_id] = *found;
  }
  return found;
}

bool QueryPeerIdentity(uint32_t ip, XNADDR* out_addr, uint32_t timeout_ms) {
  if (LookupPeer(ip, out_addr)) {
    return true;
  }
  bool ok = false;
  Exchange(kWhois, 0, {ip}, false, timeout_ms,
           [&](Reader&, const XNADDR& peer, uint64_t, uint32_t) {
             *out_addr = peer;
             ok = true;
             return true;
           });
  return ok;
}

// ---------------------------------------------------------------------------
// Search results
// ---------------------------------------------------------------------------

X_RESULT WriteSearchResults(KernelState* kernel_state,
                            const std::vector<AdvertisedSession>& sessions, uint32_t max_results,
                            uint32_t results_ptr, uint32_t results_buffer_size) {
  auto memory = kernel_state->memory();
  if (!results_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t count = std::min<uint32_t>(max_results, static_cast<uint32_t>(sessions.size()));
  uint32_t needed = sizeof(XSESSION_SEARCHRESULT_HEADER) + count * sizeof(XSESSION_SEARCHRESULT);
  if (results_buffer_size < needed) {
    count = results_buffer_size > sizeof(XSESSION_SEARCHRESULT_HEADER)
                ? (results_buffer_size - sizeof(XSESSION_SEARCHRESULT_HEADER)) /
                      sizeof(XSESSION_SEARCHRESULT)
                : 0;
  }

  auto header = memory->TranslateVirtual<XSESSION_SEARCHRESULT_HEADER*>(results_ptr);
  std::memset(header, 0, std::min<uint32_t>(results_buffer_size, needed));
  uint32_t array_ptr = results_ptr + sizeof(XSESSION_SEARCHRESULT_HEADER);
  auto results = memory->TranslateVirtual<XSESSION_SEARCHRESULT*>(array_ptr);

  // Extra data (contexts/properties/blobs) goes after the array while it
  // fits in the caller's buffer, otherwise into the system heap.
  uint32_t cursor = array_ptr + count * sizeof(XSESSION_SEARCHRESULT);
  uint32_t buffer_end = results_ptr + results_buffer_size;
  auto alloc = [&](uint32_t size) -> uint32_t {
    if (!size) return 0;
    uint32_t aligned = (cursor + 7) & ~7u;
    if (aligned + size <= buffer_end) {
      cursor = aligned + size;
      return aligned;
    }
    return memory->SystemHeapAlloc(size);
  };

  for (uint32_t i = 0; i < count; i++) {
    const auto& s = sessions[i];
    auto& out = results[i];
    std::memset(&out, 0, sizeof(out));
    U64ToXnkid(s.session_id, &out.info.sessionID);
    out.info.hostAddress = s.host;
    out.info.keyExchangeKey = s.key;
    out.open_public_slots = s.open_public;
    out.open_private_slots = s.open_private;
    out.filled_public_slots = s.max_public > s.open_public ? s.max_public - s.open_public : 0;
    out.filled_private_slots = s.max_private > s.open_private ? s.max_private - s.open_private : 0;

    std::vector<const PropertyValue*> contexts, props;
    for (const auto& p : s.properties) {
      (p.is_context ? contexts : props).push_back(&p);
    }

    out.contexts_count = static_cast<uint32_t>(contexts.size());
    uint32_t ctx_ptr = alloc(static_cast<uint32_t>(contexts.size() * sizeof(XUSER_CONTEXT)));
    out.contexts_ptr = ctx_ptr;
    for (size_t c = 0; c < contexts.size(); c++) {
      auto ctx = memory->TranslateVirtual<XUSER_CONTEXT*>(ctx_ptr) + c;
      ctx->context_id = contexts[c]->id;
      ctx->value = static_cast<uint32_t>(contexts[c]->scalar);
    }

    out.properties_count = static_cast<uint32_t>(props.size());
    uint32_t prop_ptr = alloc(static_cast<uint32_t>(props.size() * sizeof(XUSER_PROPERTY)));
    out.properties_ptr = prop_ptr;
    for (size_t p = 0; p < props.size(); p++) {
      auto prop = memory->TranslateVirtual<XUSER_PROPERTY*>(prop_ptr) + p;
      std::memset(prop, 0, sizeof(*prop));
      prop->property_id = props[p]->id;
      uint8_t type = static_cast<uint8_t>((props[p]->id >> 28) & 0xF);
      prop->data.type = type;
      uint32_t fixed = PropertySize(props[p]->id);
      if (fixed == 4) {
        prop->data.s32 = static_cast<int32_t>(static_cast<uint32_t>(props[p]->scalar));
      } else if (fixed == 8) {
        prop->data.u64 = props[p]->scalar;
      } else {
        uint32_t size = static_cast<uint32_t>(props[p]->blob.size());
        uint32_t blob_ptr = alloc(size);
        if (blob_ptr) {
          std::memcpy(memory->TranslateVirtual(blob_ptr), props[p]->blob.data(), size);
        }
        prop->data.blob.size = size;
        prop->data.blob.ptr = blob_ptr;
      }
    }
  }

  header->search_results_count = count;
  header->search_results_ptr = count ? array_ptr : 0;
  return X_ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// XSession
// ---------------------------------------------------------------------------

XSession::XSession(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

X_STATUS XSession::Initialize() {
  auto native = CreateNative(sizeof(X_KSESSION));
  if (!native) {
    return X_STATUS_NO_MEMORY;
  }
  reinterpret_cast<X_KSESSION*>(native)->handle = handle();
  return X_STATUS_SUCCESS;
}

system::object_ref<XSession> XSession::FromGuest(KernelState* kernel_state, uint32_t obj_ptr) {
  if (!obj_ptr) {
    return nullptr;
  }
  auto obj = kernel_state->memory()->TranslateVirtual<X_KSESSION*>(obj_ptr);
  return kernel_state->object_table()->LookupObject<XSession>(obj->handle);
}

X_RESULT XSession::Create(const XGI_SESSION_CREATE* data) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (created_) {
    return X_ERROR_FUNCTION_FAILED;
  }

  auto memory = kernel_state_->memory();
  auto info = memory->TranslateVirtual<XSESSION_INFO*>(data->session_info_ptr);
  auto nonce = data->nonce_ptr ? memory->TranslateVirtual<rex::be<uint64_t>*>(data->nonce_ptr)
                               : nullptr;

  uint32_t flags = data->flags;
  owner_index_ = data->user_index;
  details_ = {};
  details_.Flags = flags;
  details_.MaxPublicSlots = data->num_slots_public;
  details_.MaxPrivateSlots = data->num_slots_private;
  details_.AvailablePublicSlots = data->num_slots_public;
  details_.AvailablePrivateSlots = data->num_slots_private;
  details_.GameType = GetUserContext(owner_index_, 0x800A, 0);
  details_.GameMode = GetUserContext(owner_index_, 0x800B, 0);
  details_.eState = 0;  // lobby
  details_.UserIndexHost = kXUserIndexNone;

  std::random_device rd;
  std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^ rd());

  bool live = flags & (kSessionPresence | kSessionStats | kSessionMatchmaking | kSessionArbitration);
  is_host_ = (flags & kSessionHost) || !(flags & kSessionPeerNetwork) || flags == kSessionStats;

  if (is_host_) {
    uint8_t mask = live ? kXnkidOnlinePeer : kXnkidSystemLink;
    do {
      session_id_ = (static_cast<uint64_t>(mask) << 56) | (rng() & 0x00FFFFFFFFFFFFFFull);
    } while ((session_id_ & 0x00FFFFFFFFFFFFFFull) == 0);
    U64ToXnkid(session_id_, &info->sessionID);
    FillLocalXnAddr(&info->hostAddress);
    for (auto& b : info->keyExchangeKey.ab) b = static_cast<uint8_t>(rng());
    details_.UserIndexHost = owner_index_;
    if (nonce) *nonce = rng();
  } else {
    // Joining: session info comes from a search result or invite.
    session_id_ = XnkidToU64(info->sessionID);
    if (nonce) *nonce = rng();
  }

  details_.Nonce = nonce ? static_cast<uint64_t>(*nonce) : 0;
  details_.sessionInfo = *info;
  created_ = true;

  NetLog("XSessionCreate {} session {:016X} flags={:08X} slots={}+{}",
               is_host_ ? "host" : "client", session_id_, flags, details_.MaxPublicSlots.get(),
               details_.MaxPrivateSlots.get());

  bool advertise = is_host_ && (flags & (kSessionMatchmaking | kSessionPeerNetwork)) &&
                   (flags & kSessionHost);
  lock.unlock();
  if (advertise) {
    AdvertiseSession(this);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Delete() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deleted_) {
      return X_ERROR_SUCCESS;
    }
    deleted_ = true;
    details_.eState = 4;  // deleted
  }
  WithdrawSession(this);
  SetQosListenData(session_id_, nullptr, 0, false);
  NetLog("XSessionDelete {:016X}", session_id_);
  return X_ERROR_SUCCESS;
}

void XSession::RecountSlots() {
  uint32_t used_public = 0, used_private = 0, local = 0;
  for (const auto& [xuid, m] : members_) {
    (m.is_private ? used_private : used_public)++;
    if (m.is_local) local++;
  }
  details_.AvailablePublicSlots =
      details_.MaxPublicSlots > used_public ? details_.MaxPublicSlots - used_public : 0;
  details_.AvailablePrivateSlots =
      details_.MaxPrivateSlots > used_private ? details_.MaxPrivateSlots - used_private : 0;
  details_.ActualMemberCount = local;
  details_.ReturnedMemberCount = static_cast<uint32_t>(members_.size());
}

X_RESULT XSession::Join(const XGI_SESSION_MANAGE* data) {
  auto memory = kernel_state_->memory();
  bool local = data->xuid_array_ptr == 0;
  auto xuids = local ? nullptr : memory->TranslateVirtual<rex::be<uint64_t>*>(data->xuid_array_ptr);
  auto indices = data->indices_array_ptr
                     ? memory->TranslateVirtual<rex::be<uint32_t>*>(data->indices_array_ptr)
                     : nullptr;
  auto privates = data->private_slots_array_ptr
                      ? memory->TranslateVirtual<rex::be<uint32_t>*>(data->private_slots_array_ptr)
                      : nullptr;

  std::lock_guard<std::mutex> lock(mutex_);
  for (uint32_t i = 0; i < data->array_count; i++) {
    Member m = {};
    if (local) {
      m.user_index = indices ? static_cast<uint32_t>(indices[i]) : 0;
      m.xuid = LocalXuid(m.user_index);
      m.is_local = true;
    } else {
      m.xuid = xuids[i];
      m.user_index = kXUserIndexNone;
      for (uint32_t u = 0; u < kXUserMaxUserCount; u++) {
        if (LocalXuid(u) == m.xuid) {
          m.user_index = u;
          m.is_local = true;
        }
      }
    }
    bool want_private = privates && privates[i];
    uint32_t used_private = 0;
    for (const auto& [x, mm] : members_) used_private += mm.is_private;
    m.is_private = want_private && used_private < details_.MaxPrivateSlots;
    if (!members_.count(m.xuid)) {
      members_[m.xuid] = m;
      NetLog("Session {:016X}: {} member {:016X} joined ({})", session_id_,
                   m.is_local ? "local" : "remote", m.xuid, m.is_private ? "private" : "public");
    }
  }
  RecountSlots();
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Leave(const XGI_SESSION_MANAGE* data) {
  auto memory = kernel_state_->memory();
  bool local = data->xuid_array_ptr == 0;
  auto xuids = local ? nullptr : memory->TranslateVirtual<rex::be<uint64_t>*>(data->xuid_array_ptr);
  auto indices = data->indices_array_ptr
                     ? memory->TranslateVirtual<rex::be<uint32_t>*>(data->indices_array_ptr)
                     : nullptr;

  std::lock_guard<std::mutex> lock(mutex_);
  for (uint32_t i = 0; i < data->array_count; i++) {
    uint64_t xuid = local ? LocalXuid(indices ? static_cast<uint32_t>(indices[i]) : 0)
                          : static_cast<uint64_t>(xuids[i]);
    if (members_.erase(xuid)) {
      NetLog("Session {:016X}: member {:016X} left", session_id_, xuid);
    }
  }
  RecountSlots();
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Modify(const XGI_SESSION_MODIFY* data) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t modifiable = kSessionModifiersMask | kSessionArbitration;
  details_.Flags = (details_.Flags & ~modifiable) | (data->flags & modifiable);
  details_.MaxPublicSlots = data->maxPublicSlots;
  details_.MaxPrivateSlots = data->maxPrivateSlots;
  RecountSlots();
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetDetails(const XGI_SESSION_DETAILS* data) {
  auto memory = kernel_state_->memory();
  std::lock_guard<std::mutex> lock(mutex_);

  uint32_t buffer_size = sizeof(XSESSION_LOCAL_DETAILS);
  if (data->details_buffer_size_ptr) {
    buffer_size = *memory->TranslateVirtual<rex::be<uint32_t>*>(data->details_buffer_size_ptr);
  }
  auto out = memory->TranslateVirtual<XSESSION_LOCAL_DETAILS*>(data->session_details_ptr);
  *out = details_;

  uint32_t capacity = buffer_size > sizeof(XSESSION_LOCAL_DETAILS)
                          ? (buffer_size - sizeof(XSESSION_LOCAL_DETAILS)) / sizeof(XSESSION_MEMBER)
                          : 0;
  uint32_t members_ptr = data->session_details_ptr + sizeof(XSESSION_LOCAL_DETAILS);
  auto members = memory->TranslateVirtual<XSESSION_MEMBER*>(members_ptr);
  uint32_t n = 0;
  for (const auto& [xuid, m] : members_) {
    if (n >= capacity) break;
    members[n].OnlineXUID = m.xuid;
    members[n].UserIndex = m.user_index;
    members[n].Flags = m.is_private ? 1 : 0;
    n++;
  }
  out->ReturnedMemberCount = n;
  out->SessionMembers_ptr = n ? members_ptr : 0;
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Migrate(const XGI_SESSION_MIGRATE* data) {
  auto memory = kernel_state_->memory();
  auto info = memory->TranslateVirtual<XSESSION_INFO*>(data->session_info_ptr);
  bool become_host;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    become_host = data->user_index != kXUserIndexNone;
    if (become_host) {
      is_host_ = true;
      details_.UserIndexHost = data->user_index;
      U64ToXnkid(session_id_, &info->sessionID);
      FillLocalXnAddr(&info->hostAddress);
      info->keyExchangeKey = details_.sessionInfo.keyExchangeKey;
    } else if (auto known = FindKnownSession(session_id_)) {
      U64ToXnkid(session_id_, &info->sessionID);
      info->hostAddress = known->host;
      info->keyExchangeKey = known->key;
    } else {
      *info = details_.sessionInfo;
    }
    details_.sessionInfo = *info;
    details_.xnkidArbitration = info->sessionID;
  }
  NetLog("XSessionMigrateHost {:016X} ({})", session_id_,
               become_host ? "we are host" : "remote host");
  if (become_host) {
    AdvertiseSession(this);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::RegisterArbitration(const XGI_SESSION_ARBITRATION* data) {
  auto memory = kernel_state_->memory();
  std::lock_guard<std::mutex> lock(mutex_);

  // One registrant per machine. Remote machines are identified by XUID.
  std::vector<std::pair<uint64_t, std::vector<uint64_t>>> machines;
  std::vector<uint64_t> local_users;
  for (const auto& [xuid, m] : members_) {
    if (m.is_local) {
      local_users.push_back(xuid);
    } else {
      machines.push_back({0xFB00000000000000ull | (xuid & 0x0000FFFFFFFFFFFFull), {xuid}});
    }
  }
  if (!local_users.empty() || machines.empty()) {
    if (local_users.empty()) local_users.push_back(LocalXuid(owner_index_));
    machines.insert(machines.begin(), {LocalMachineId(), local_users});
  }

  uint32_t base = data->results_ptr;
  uint32_t end = base + data->results_buffer_size;
  auto results = memory->TranslateVirtual<XSESSION_REGISTRATION_RESULTS*>(base);
  uint32_t reg_ptr = base + sizeof(XSESSION_REGISTRATION_RESULTS);
  uint32_t xuid_ptr = reg_ptr + static_cast<uint32_t>(machines.size() * sizeof(XSESSION_REGISTRANT));
  auto alloc = [&](uint32_t size) {
    if (xuid_ptr + size <= end) {
      uint32_t p = xuid_ptr;
      xuid_ptr += size;
      return p;
    }
    return memory->SystemHeapAlloc(size);
  };
  if (reg_ptr + machines.size() * sizeof(XSESSION_REGISTRANT) > end) {
    reg_ptr = memory->SystemHeapAlloc(
        static_cast<uint32_t>(machines.size() * sizeof(XSESSION_REGISTRANT)));
  }

  auto regs = memory->TranslateVirtual<XSESSION_REGISTRANT*>(reg_ptr);
  for (size_t i = 0; i < machines.size(); i++) {
    uint32_t users = alloc(static_cast<uint32_t>(machines[i].second.size() * 8));
    auto u = memory->TranslateVirtual<rex::be<uint64_t>*>(users);
    for (size_t j = 0; j < machines[i].second.size(); j++) u[j] = machines[i].second[j];
    regs[i].machine_id = machines[i].first;
    regs[i].trustworthiness = 1;
    regs[i].num_users = static_cast<uint32_t>(machines[i].second.size());
    regs[i].users_ptr = users;
  }
  results->registrants_count = static_cast<uint32_t>(machines.size());
  results->registrants_ptr = reg_ptr;
  details_.eState = 1;  // registration
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  details_.eState = 2;  // in game
  NetLog("XSessionStart {:016X}", session_id_);
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::End() {
  std::lock_guard<std::mutex> lock(mutex_);
  details_.eState = 3;  // reporting
  NetLog("XSessionEnd {:016X}", session_id_);
  return X_ERROR_SUCCESS;
}

AdvertisedSession XSession::Snapshot() const {
  AdvertisedSession s;
  std::lock_guard<std::mutex> lock(mutex_);
  s.session_id = session_id_;
  s.key = details_.sessionInfo.keyExchangeKey;
  s.flags = details_.Flags;
  s.max_public = details_.MaxPublicSlots;
  s.max_private = details_.MaxPrivateSlots;
  s.open_public = details_.AvailablePublicSlots;
  s.open_private = details_.AvailablePrivateSlots;
  s.properties = GetUserProperties(owner_index_);
  {
    auto& st = state();
    std::lock_guard<std::mutex> st_lock(st.mutex);
    auto it = st.qos_listen.find(session_id_);
    if (it != st.qos_listen.end()) s.qos_data = it->second;
  }
  return s;
}

}  // namespace rex::kernel::xam::live
