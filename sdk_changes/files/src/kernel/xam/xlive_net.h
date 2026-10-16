#pragma once
/**
 ******************************************************************************
 * ReXGlue : Xbox LIVE / System Link emulation for peer-to-peer play           *
 ******************************************************************************
 * Based on the Xenia Canary netplay fork (AdrianCassar/xenia-canary),         *
 * released under the BSD license. Instead of a matchmaking web service this   *
 * implementation uses a small UDP discovery protocol: a host answers session  *
 * queries directly, clients find it by LAN broadcast or by an explicit IP.    *
 ******************************************************************************
 */

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <rex/types.h>

#include <fmt/format.h>

namespace rex::kernel::xam::live {

// ---------------------------------------------------------------------------
// Guest structures (big-endian, layout must match the console)
// ---------------------------------------------------------------------------

struct XNKID {
  uint8_t ab[8];
};
static_assert(sizeof(XNKID) == 0x8);

struct XNKEY {
  uint8_t ab[16];
};
static_assert(sizeof(XNKEY) == 0x10);

struct XNADDR {
  uint32_t ina;        // network byte order
  uint32_t inaOnline;  // network byte order
  rex::be<uint16_t> wPortOnline;
  uint8_t abEnet[6];
  uint8_t abOnline[20];
};
static_assert(sizeof(XNADDR) == 0x24);

struct XSESSION_INFO {
  XNKID sessionID;
  XNADDR hostAddress;
  XNKEY keyExchangeKey;
};
static_assert(sizeof(XSESSION_INFO) == 0x3C);

struct XSESSION_SEARCHRESULT {
  XSESSION_INFO info;
  rex::be<uint32_t> open_public_slots;
  rex::be<uint32_t> open_private_slots;
  rex::be<uint32_t> filled_public_slots;
  rex::be<uint32_t> filled_private_slots;
  rex::be<uint32_t> properties_count;
  rex::be<uint32_t> contexts_count;
  rex::be<uint32_t> properties_ptr;
  rex::be<uint32_t> contexts_ptr;
};
static_assert(sizeof(XSESSION_SEARCHRESULT) == 0x5C);

struct XSESSION_SEARCHRESULT_HEADER {
  rex::be<uint32_t> search_results_count;
  rex::be<uint32_t> search_results_ptr;
};
static_assert(sizeof(XSESSION_SEARCHRESULT_HEADER) == 0x8);

struct XSESSION_LOCAL_DETAILS {
  rex::be<uint32_t> UserIndexHost;
  rex::be<uint32_t> GameType;
  rex::be<uint32_t> GameMode;
  rex::be<uint32_t> Flags;
  rex::be<uint32_t> MaxPublicSlots;
  rex::be<uint32_t> MaxPrivateSlots;
  rex::be<uint32_t> AvailablePublicSlots;
  rex::be<uint32_t> AvailablePrivateSlots;
  rex::be<uint32_t> ActualMemberCount;
  rex::be<uint32_t> ReturnedMemberCount;
  rex::be<uint32_t> eState;
  rex::be<uint64_t> Nonce;
  XSESSION_INFO sessionInfo;
  XNKID xnkidArbitration;
  rex::be<uint32_t> SessionMembers_ptr;
};
static_assert(sizeof(XSESSION_LOCAL_DETAILS) == 0x80);

struct XSESSION_MEMBER {
  rex::be<uint64_t> OnlineXUID;
  rex::be<uint32_t> UserIndex;
  rex::be<uint32_t> Flags;
};
static_assert(sizeof(XSESSION_MEMBER) == 0x10);

struct XSESSION_REGISTRANT {
  rex::be<uint64_t> machine_id;
  rex::be<uint32_t> trustworthiness;
  rex::be<uint32_t> num_users;
  rex::be<uint32_t> users_ptr;
};
static_assert(sizeof(XSESSION_REGISTRANT) == 0x18);

struct XSESSION_REGISTRATION_RESULTS {
  rex::be<uint32_t> registrants_count;
  rex::be<uint32_t> registrants_ptr;
};
static_assert(sizeof(XSESSION_REGISTRATION_RESULTS) == 0x8);

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
};
static_assert(sizeof(XNQOSINFO) == 0x18);

struct XNQOS {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
};

// XUSER_PROPERTY / X_USER_DATA as the guest sees them.
struct X_USER_DATA {
  uint8_t type;
  uint8_t pad[7];
  union {
    rex::be<int32_t> s32;
    rex::be<int64_t> s64;
    rex::be<uint64_t> u64;
    struct {
      rex::be<uint32_t> size;
      rex::be<uint32_t> ptr;
    } blob;
  };
};
static_assert(sizeof(X_USER_DATA) == 0x10);

struct XUSER_PROPERTY {
  rex::be<uint32_t> property_id;
  uint32_t pad;
  X_USER_DATA data;
};
static_assert(sizeof(XUSER_PROPERTY) == 0x18);

struct XUSER_CONTEXT {
  rex::be<uint32_t> context_id;
  rex::be<uint32_t> value;
};
static_assert(sizeof(XUSER_CONTEXT) == 0x8);

// XGI message payloads
struct XGI_SESSION_CREATE {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> flags;
  rex::be<uint32_t> num_slots_public;
  rex::be<uint32_t> num_slots_private;
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> session_info_ptr;
  rex::be<uint32_t> nonce_ptr;
};
static_assert(sizeof(XGI_SESSION_CREATE) == 0x1C);

struct XGI_SESSION_STATE {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> flags;
  rex::be<uint64_t> session_nonce;
};
static_assert(sizeof(XGI_SESSION_STATE) == 0x10);

struct XGI_SESSION_MANAGE {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> array_count;
  rex::be<uint32_t> xuid_array_ptr;
  rex::be<uint32_t> indices_array_ptr;
  rex::be<uint32_t> private_slots_array_ptr;
};
static_assert(sizeof(XGI_SESSION_MANAGE) == 0x14);

struct XGI_SESSION_MODIFY {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> flags;
  rex::be<uint32_t> maxPublicSlots;
  rex::be<uint32_t> maxPrivateSlots;
};
static_assert(sizeof(XGI_SESSION_MODIFY) == 0x10);

struct XGI_SESSION_SEARCH {
  rex::be<uint32_t> proc_index;
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> num_results;
  rex::be<uint16_t> num_props;
  rex::be<uint16_t> num_ctx;
  rex::be<uint32_t> props_ptr;
  rex::be<uint32_t> ctx_ptr;
  rex::be<uint32_t> results_buffer_size;
  rex::be<uint32_t> search_results_ptr;
};
static_assert(sizeof(XGI_SESSION_SEARCH) == 0x20);

struct XGI_SESSION_SEARCH_WEIGHTED {
  rex::be<uint32_t> proc_index;
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> num_results;
  rex::be<uint16_t> num_weighted_properties;
  rex::be<uint16_t> num_weighted_contexts;
  rex::be<uint32_t> weighted_search_properties_ptr;
  rex::be<uint32_t> weighted_search_contexts_ptr;
  rex::be<uint16_t> num_props;
  rex::be<uint16_t> num_ctx;
  rex::be<uint32_t> non_weighted_search_properties_ptr;
  rex::be<uint32_t> non_weighted_search_contexts_ptr;
  rex::be<uint32_t> results_buffer_size;
  rex::be<uint32_t> search_results_ptr;
  rex::be<uint32_t> num_users;
  rex::be<uint32_t> weighted_search;
};
static_assert(sizeof(XGI_SESSION_SEARCH_WEIGHTED) == 0x34);

struct XGI_SESSION_SEARCH_BYID {
  rex::be<uint32_t> user_index;
  XNKID session_id;
  rex::be<uint32_t> results_buffer_size;
  rex::be<uint32_t> search_results_ptr;
};
static_assert(sizeof(XGI_SESSION_SEARCH_BYID) == 0x14);

struct XGI_SESSION_SEARCH_BYIDS {
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> num_session_ids;
  rex::be<uint32_t> session_ids_ptr;
  rex::be<uint32_t> results_buffer_size;
  rex::be<uint32_t> search_results_ptr;
  rex::be<uint32_t> reserved1;
  rex::be<uint32_t> reserved2;
  rex::be<uint32_t> reserved3;
};
static_assert(sizeof(XGI_SESSION_SEARCH_BYIDS) == 0x20);

struct XGI_SESSION_DETAILS {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> details_buffer_size_ptr;
  rex::be<uint32_t> session_details_ptr;
  rex::be<uint32_t> reserved1;
  rex::be<uint32_t> reserved2;
  rex::be<uint32_t> reserved3;
};
static_assert(sizeof(XGI_SESSION_DETAILS) == 0x18);

struct XGI_SESSION_MIGRATE {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> session_info_ptr;
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> reserved1;
  rex::be<uint32_t> reserved2;
  rex::be<uint32_t> reserved3;
};
static_assert(sizeof(XGI_SESSION_MIGRATE) == 0x18);

struct XGI_SESSION_ARBITRATION {
  rex::be<uint32_t> obj_ptr;
  rex::be<uint32_t> flags;
  rex::be<uint64_t> session_nonce;
  rex::be<uint32_t> session_duration_sec;
  rex::be<uint32_t> results_buffer_size;
  rex::be<uint32_t> results_ptr;
};
static_assert(sizeof(XGI_SESSION_ARBITRATION) == 0x20);

// Guest-visible session kernel object (XAM only stores the handle).
struct X_KSESSION {
  rex::be<uint32_t> handle;
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

enum SessionFlags : uint32_t {
  kSessionHost = 0x01,
  kSessionPresence = 0x02,
  kSessionStats = 0x04,
  kSessionMatchmaking = 0x08,
  kSessionArbitration = 0x10,
  kSessionPeerNetwork = 0x20,
  kSessionModifiersMask = 0x0F80,
};

constexpr uint8_t kXnkidSystemLink = 0x00;
constexpr uint8_t kXnkidOnlinePeer = 0x80;

constexpr uint32_t kXUserIndexNone = 0xFE;
constexpr uint32_t kXUserMaxUserCount = 4;

// ---------------------------------------------------------------------------
// Session / property data shared with peers
// ---------------------------------------------------------------------------

// A property value captured from XUserSetProperty / XUserSetContext. Blob
// payloads (strings, binary) are kept in guest byte order.
struct PropertyValue {
  uint32_t id = 0;
  uint64_t scalar = 0;           // raw big-endian value for fixed-size types
  std::vector<uint8_t> blob;     // WSTRING / BINARY payload
  bool is_context = false;
};

// Everything a client needs to fill an XSESSION_SEARCHRESULT and QoS data.
struct AdvertisedSession {
  uint64_t session_id = 0;
  XNKEY key = {};
  XNADDR host = {};
  uint64_t host_xuid = 0;
  uint32_t flags = 0;
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  uint32_t open_public = 0;
  uint32_t open_private = 0;
  std::vector<PropertyValue> properties;  // properties and contexts
  std::vector<uint8_t> qos_data;
  uint32_t rtt_ms = 0;  // measured by the searching client
};

// Appends a line to net.log (next to the executable) and the kernel log.
void NetLogLine(const std::string& line);
template <typename... Args>
void NetLog(fmt::format_string<Args...> f, Args&&... args) {
  NetLogLine(fmt::format(f, std::forward<Args>(args)...));
}

// ---------------------------------------------------------------------------
// Identity & addressing
// ---------------------------------------------------------------------------

// Initializes WinSock, identity and the discovery responder (idempotent).
void EnsureStarted();

// Local IPv4 (network byte order) used in our XNADDR.
uint32_t LocalIp();
// IPv4 (network byte order) the game should bind INADDR_ANY sockets to, or 0.
uint32_t BindIp();
uint64_t LocalMachineId();
uint64_t LocalXuid(uint32_t user_index);
std::string LocalGamertag();
void FillLocalXnAddr(XNADDR* addr);
bool IsLocalMac(const uint8_t mac[6]);

// Peer registry: maps a peer's IPv4 (network order) to its XNADDR/XUID.
void RememberPeer(uint32_t ip, const XNADDR& addr, uint64_t xuid);
bool LookupPeer(uint32_t ip, XNADDR* out_addr);

// ---------------------------------------------------------------------------
// Properties, QoS and advertisement
// ---------------------------------------------------------------------------

void SetUserContext(uint32_t user_index, uint32_t id, uint32_t value);
void SetUserProperty(uint32_t user_index, uint32_t id, const uint8_t* guest_data,
                     uint32_t size);
std::vector<PropertyValue> GetUserProperties(uint32_t user_index);
uint32_t GetUserContext(uint32_t user_index, uint32_t id, uint32_t default_value);

void SetQosListenData(uint64_t session_id, const uint8_t* data, uint32_t size, bool enable);
std::optional<AdvertisedSession> FindKnownSession(uint64_t session_id);

class XSession;
void AdvertiseSession(XSession* session);
void WithdrawSession(XSession* session);

// Queries LAN broadcast and the configured host for sessions.
std::vector<AdvertisedSession> SearchSessions(uint32_t timeout_ms);
std::optional<AdvertisedSession> QuerySessionById(uint64_t session_id, uint32_t timeout_ms);
// Resolves the XNADDR of a peer by asking it directly.
bool QueryPeerIdentity(uint32_t ip, XNADDR* out_addr, uint32_t timeout_ms);

// ---------------------------------------------------------------------------
// Session kernel object
// ---------------------------------------------------------------------------

inline uint64_t XnkidToU64(const XNKID& id) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | id.ab[i];
  return v;
}

inline void U64ToXnkid(uint64_t v, XNKID* id) {
  for (int i = 7; i >= 0; i--) {
    id->ab[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

class XSession : public system::XObject {
 public:
  static const system::XObject::Type kObjectType = system::XObject::Type::Session;

  explicit XSession(system::KernelState* kernel_state);

  X_STATUS Initialize();

  X_RESULT Create(const XGI_SESSION_CREATE* data);
  X_RESULT Delete();
  X_RESULT Join(const XGI_SESSION_MANAGE* data);
  X_RESULT Leave(const XGI_SESSION_MANAGE* data);
  X_RESULT Modify(const XGI_SESSION_MODIFY* data);
  X_RESULT GetDetails(const XGI_SESSION_DETAILS* data);
  X_RESULT Migrate(const XGI_SESSION_MIGRATE* data);
  X_RESULT RegisterArbitration(const XGI_SESSION_ARBITRATION* data);
  X_RESULT Start();
  X_RESULT End();

  bool is_host() const { return is_host_; }
  bool is_created() const { return created_; }
  uint64_t session_id() const { return session_id_; }
  uint32_t flags() const { return details_.Flags; }

  // Snapshot for the discovery responder.
  AdvertisedSession Snapshot() const;

  static system::object_ref<XSession> FromGuest(system::KernelState* kernel_state, uint32_t obj_ptr);

 private:
  void RecountSlots();

  mutable std::mutex mutex_;
  bool created_ = false;
  bool is_host_ = false;
  bool deleted_ = false;
  uint64_t session_id_ = 0;
  uint32_t owner_index_ = 0;
  XSESSION_LOCAL_DETAILS details_ = {};
  // xuid -> member (host order fields)
  struct Member {
    uint64_t xuid;
    uint32_t user_index;
    bool is_private;
    bool is_local;
  };
  std::map<uint64_t, Member> members_;
};

// Search result helpers used by the XGI app.
X_RESULT WriteSearchResults(system::KernelState* kernel_state,
                            const std::vector<AdvertisedSession>& sessions, uint32_t max_results,
                            uint32_t results_ptr, uint32_t results_buffer_size);

}  // namespace rex::kernel::xam::live
