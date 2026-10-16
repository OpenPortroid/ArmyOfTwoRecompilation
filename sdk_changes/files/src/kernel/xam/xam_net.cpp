/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - Peer-to-peer online play (XNet addressing, keys, QoS,
 *              overlapped socket I/O), based on the Xenia Canary netplay fork
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/hook.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "xlive_net.h"

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#include <ws2tcpip.h>
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
using live::XNADDR;
using live::XNKEY;
using live::XNKID;
using live::XNQOS;
using live::XNQOSINFO;

namespace {

constexpr uint32_t kWsaNotSock = 0x2736;
constexpr uint32_t kWsaEFault = 0x271E;
constexpr uint32_t kWsaEInval = 0x2726;

void SetSocketError() {
  XThread::SetLastError(XSocket::LastError());
}

object_ref<XSocket> LookupSocket(uint32_t handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(handle);
  if (!socket) {
    XThread::SetLastError(kWsaNotSock);
  }
  return socket;
}

// Binds an unbound socket to the configured interface so that packets carry
// the right source address (needed when several adapters/instances exist).
void EnsureImplicitBind(XSocket* socket) {
  uint32_t bind_ip = live::BindIp();
  if (!bind_ip || socket->bound()) {
    return;
  }
  XSOCKADDR_IN addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr = bind_ip;
  socket->Bind(&addr, sizeof(addr));
}

void SignalEventHandle(uint32_t handle) {
  if (!handle) {
    return;
  }
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(handle);
  if (ev) {
    ev->Set(0, false);
  }
}

// IN_ADDR values passed by value in a register hold the address as a
// big-endian integer; convert to the in-memory network-order form.
uint32_t RegisterToNetworkIp(uint32_t value) {
  return htonl(value);
}

std::string IpString(uint32_t network_ip) {
  in_addr a = {};
  a.s_addr = network_ip;
  char buf[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

// Every 5 s: a line per socket that sent or received since the last report.
void ReportTraffic() {
  static std::atomic<int64_t> next_report{0};
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  int64_t due = next_report.load();
  if (now < due || !next_report.compare_exchange_strong(due, now + 5000)) {
    return;
  }
  for (auto& socket : REX_KERNEL_OBJECTS()->GetObjectsByType<XSocket>()) {
    auto& t = socket->traffic();
    uint64_t sent = t.sent_packets, recv = t.recv_packets;
    uint64_t errors = t.send_errors + t.recv_errors;
    if (sent == t.reported_sent && recv == t.reported_recv && errors == t.reported_errors) {
      continue;
    }
    live::NetLog(
        "traffic {:08X} {}:{} sent {} (+{}) pkts {} B, last to {}:{} | recv {} (+{}) pkts {} B, "
        "last from {}:{} | errors send {} recv {}",
        socket->handle(), socket->is_vdp() ? "vdp" : "ip", socket->bound_port(), sent,
        sent - t.reported_sent, t.sent_bytes.load(), IpString(t.last_to_ip), t.last_to_port.load(),
        recv, recv - t.reported_recv, t.recv_bytes.load(), IpString(t.last_from_ip),
        t.last_from_port.load(), t.send_errors.load(), t.recv_errors.load());
    t.reported_sent = sent;
    t.reported_recv = recv;
    t.reported_errors = errors;
  }
}

// Logs the first few calls of each export and any call that blocks > 200 ms.
struct NetCallTrace {
  const char* name;
  std::chrono::steady_clock::time_point start;
  explicit NetCallTrace(const char* n) : name(n), start(std::chrono::steady_clock::now()) {
    {
      static std::mutex m;
      static std::map<std::string, int> counts;
      std::lock_guard<std::mutex> lock(m);
      if (++counts[name] <= 3) {
        live::NetLog("call {}", name);
      }
    }
    ReportTraffic();
  }
  ~NetCallTrace() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    if (ms > 200) {
      live::NetLog("SLOW {} took {} ms", name, ms);
    }
  }
};
#define NET_TRACE() NetCallTrace net_call_trace_(__func__)

}  // namespace

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  NET_TRACE();
  if (params) {
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }
  live::NetLog("XNetStartup");
  live::EnsureStarted();
  return 0;
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  NET_TRACE();
  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  NET_TRACE();
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return kWsaEInval;
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  NET_TRACE();
  static std::mt19937 rng{std::random_device{}()};
  auto out = static_cast<uint8_t*>(static_cast<void*>(buffer_ptr));
  for (uint32_t i = 0; i < length; i++) {
    out[i] = static_cast<uint8_t>(rng());
  }
  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
  NET_TRACE();
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  live::EnsureStarted();
  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  NET_TRACE();
  // This does nothing. We need WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  NET_TRACE();
  return XThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket_handle, u32 buffers_ptr, u32 buffer_count,
                             u32 num_bytes_recv_ptr, u32 flags_ptr, u32 from_ptr,
                             u32 fromlen_ptr, u32 overlapped_ptr, u32 completion_routine_ptr) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  int ret = socket->WSARecvFrom(buffers_ptr, buffer_count, num_bytes_recv_ptr, flags_ptr,
                                from_ptr, fromlen_ptr, overlapped_ptr);
  return static_cast<u32>(ret);
}

u32 NetDll_WSARecv_entry(u32 caller, u32 socket_handle, u32 buffers_ptr, u32 buffer_count,
                         u32 num_bytes_recv_ptr, u32 flags_ptr, u32 overlapped_ptr,
                         u32 completion_routine_ptr) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return ~0u;
  }
  int ret = socket->WSARecvFrom(buffers_ptr, buffer_count, num_bytes_recv_ptr, flags_ptr, 0, 0,
                                overlapped_ptr);
  return static_cast<u32>(ret);
}

u32 NetDll_WSAGetOverlappedResult_entry(u32 caller, u32 socket_handle, u32 overlapped_ptr,
                                        u32 bytes_ptr, u32 wait, u32 flags_ptr) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket || !overlapped_ptr) {
    return 0;
  }
  return socket->WSAGetOverlappedResult(overlapped_ptr, bytes_ptr, wait != 0, flags_ptr) ? 1 : 0;
}

u32 NetDll_WSACancelOverlappedIO_entry(u32 caller, u32 socket_handle) {
  NET_TRACE();
  return 0;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return ~0u;
  }

  std::vector<uint8_t> combined;
  for (uint32_t i = 0; i < num_buffers; i++) {
    auto src = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(buffers[i].buf_ptr);
    combined.insert(combined.end(), src, src + buffers[i].len);
  }

  EnsureImplicitBind(socket.get());
  const XSOCKADDR_IN* to = to_ptr ? static_cast<const XSOCKADDR_IN*>(to_ptr) : nullptr;
  int ret = socket->SendTo(combined.data(), static_cast<uint32_t>(combined.size()), 0, to);
  if (ret < 0) {
    SetSocketError();
    if (overlapped) {
      overlapped->internal = XSocket::LastError();
    }
    return ~0u;
  }

  if (num_bytes_sent) {
    *num_bytes_sent = static_cast<uint32_t>(ret);
  }
  if (overlapped) {
    overlapped->internal = 0;
    overlapped->internal_high = static_cast<uint32_t>(ret);
    overlapped->offset = 0;
    SignalEventHandle(overlapped->event_handle);
  }
  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  NET_TRACE();
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  // NT wait types are the reverse of fWaitAll (WaitAll = 0, WaitAny = 1), and
  // the timeout is in milliseconds while NT takes negative (relative) 100 ns
  // units. Waiting for all events instead of any deadlocked EA's DirtySock:
  // its receive thread holds a lock while it waits for a wake-up event or
  // socket I/O, and closing a socket sets the wake-up event and takes the lock.
  uint32_t wait_type = wait_all ? 0 : 1;
  uint64_t timeout_wait = static_cast<uint64_t>(-static_cast<int64_t>(timeout) * 10000);

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_type, 1, alertable,
                                                    timeout != ~0u ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return result;
}

u32 NetDll_WSACreateEvent_entry() {
  NET_TRACE();
  auto ev = object_ref<XEvent>(new XEvent(REX_KERNEL_STATE()));
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  NET_TRACE();
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  NET_TRACE();
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  NET_TRACE();
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  NET_TRACE();
  live::FillLocalXnAddr(addr_ptr);
  return XnAddrStatus::XNET_GET_XNADDR_ETHERNET | XnAddrStatus::XNET_GET_XNADDR_STATIC |
         XnAddrStatus::XNET_GET_XNADDR_GATEWAY | XnAddrStatus::XNET_GET_XNADDR_DNS |
         XnAddrStatus::XNET_GET_XNADDR_ONLINE;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  NET_TRACE();
  addr_ptr.Zero();
  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr,
                                       mapped_u64 id_ptr) {
  NET_TRACE();
  if (!addr_ptr || !id_ptr) {
    return kWsaEInval;
  }
  uint64_t id = 0;
  for (int i = 0; i < 8; i++) {
    id = (id << 8) | addr_ptr->abOnline[8 + i];
  }
  if (!id) {
    // Derive from the MAC for peers that did not provide one.
    id = 0xFA00000000000000ull;
    for (int i = 0; i < 6; i++) {
      id |= static_cast<uint64_t>(addr_ptr->abEnet[i]) << (8 * (5 - i));
    }
  }
  *id_ptr = id;
  return 0;
}

u32 NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                    u32 string_size) {
  NET_TRACE();
  rex::string::copy_truncating(string_out, IpString(RegisterToNetworkIp(in_addr)), string_size);
  return 0;
}

// Converts an XNADDR into the IN_ADDR the title then uses with sockets. For
// peers this is simply their real (or VPN) IPv4 address.
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_u32 in_addr_ptr) {
  NET_TRACE();
  if (!xn_addr || !in_addr_ptr) {
    return kWsaEInval;
  }
  uint32_t ip;
  if (live::IsLocalMac(xn_addr->abEnet)) {
    ip = live::LocalIp();
  } else {
    ip = xn_addr->ina ? xn_addr->ina : xn_addr->inaOnline;
    XNADDR known;
    if (!live::LookupPeer(ip, &known)) {
      live::RememberPeer(ip, *xn_addr, 0);
    }
  }
  std::memcpy(static_cast<void*>(in_addr_ptr), &ip, 4);
  live::NetLog("XNetXnAddrToInAddr -> {}", IpString(ip));
  return 0;
}

u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    ppc_ptr_t<XNKID> xid) {
  NET_TRACE();
  uint32_t ip = RegisterToNetworkIp(in_addr);
  XNADDR result = {};
  bool is_self = ip == live::LocalIp() || ip == htonl(INADDR_LOOPBACK) || ip == INADDR_BROADCAST;
  if (is_self) {
    live::FillLocalXnAddr(&result);
  } else if (!live::QueryPeerIdentity(ip, &result, 500)) {
    live::NetLog("WARNING: XNetInAddrToXnAddr: unknown peer {}", IpString(ip));
    result.ina = ip;
    result.inaOnline = ip;
    result.wPortOnline = 3074;
    result.abEnet[0] = 0x02;
    std::memcpy(&result.abEnet[2], &ip, 4);
  }
  if (xn_addr) {
    *xn_addr = result;
  }
  live::NetLog("XNetInAddrToXnAddr({})", IpString(ip));
  if (xid) {
    std::memset(xid, 0, sizeof(XNKID));
  }
  return 0;
}

u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  NET_TRACE();
  return 0;
}

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  NET_TRACE();
  // ACTIVE | 100MBPS | FULL_DUPLEX
  return 0x01 | 0x02 | 0x08;
}

u32 NetDll_XNetConnect_entry(u32 caller, u32 in_addr) {
  NET_TRACE();
  live::NetLog("XNetConnect({})", IpString(RegisterToNetworkIp(in_addr)));
  return 0;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 in_addr) {
  NET_TRACE();
  return 2;  // XNET_CONNECT_STATUS_CONNECTED
}

u32 NetDll_XNetCreateKey_entry(u32 caller, ppc_ptr_t<XNKID> xnkid, ppc_ptr_t<XNKEY> xnkey) {
  NET_TRACE();
  live::NetLog("XNetCreateKey");
  static std::mt19937_64 rng{std::random_device{}()};
  if (xnkid) {
    uint64_t id;
    do {
      id = rng() & 0x00FFFFFFFFFFFFFFull;  // system link session
    } while (!id);
    live::U64ToXnkid(id, xnkid);
  }
  if (xnkey) {
    for (auto& b : xnkey->ab) b = static_cast<uint8_t>(rng());
  }
  return 0;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, ppc_ptr_t<XNKID> xnkid, ppc_ptr_t<XNKEY> xnkey) {
  NET_TRACE();
  return 0;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, ppc_ptr_t<XNKID> xnkid) {
  NET_TRACE();
  return 0;
}

u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 in_addr) {
  NET_TRACE();
  return 0;
}

u32 NetDll_XNetServerToInAddr_entry(u32 caller, u32 server_addr, u32 service_id,
                                    mapped_u32 pina) {
  NET_TRACE();
  if (!pina) {
    return kWsaEInval;
  }
  live::NetLog("XNetServerToInAddr({}, service={:08X})", IpString(RegisterToNetworkIp(server_addr)),
               service_id);
  *pina = server_addr;
  return 0;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle,
                               mapped_u32 pdns) {
  NET_TRACE();
  live::NetLog("XNetDnsLookup('{}') - not resolving", host ? host.value() : "");
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    std::memset(dns, 0, sizeof(XNDNS));
    dns->status = 11001;  // WSAHOST_NOT_FOUND
    *pdns = dns_guest;
  }
  SignalEventHandle(event_handle);
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  NET_TRACE();
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  NET_TRACE();
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    std::memset(qos, 0, sizeof(XNQOS));
    *pqos = qos_guest;
  }
  SignalEventHandle(event_handle);
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  NET_TRACE();
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, ppc_ptr_t<XNKID> xnkid, mapped_void data, u32 data_size,
                               u32 bits_per_sec, u32 flags) {
  NET_TRACE();
  if (!xnkid) {
    return kWsaEInval;
  }
  constexpr uint32_t kEnable = 0x01, kDisable = 0x02, kSetData = 0x04, kRelease = 0x10;
  uint64_t session_id = live::XnkidToU64(*xnkid);
  live::NetLog("XNetQosListen({:016X}, size={}, flags={:X})", session_id, data_size, flags);
  if (flags & (kDisable | kRelease)) {
    live::SetQosListenData(session_id, nullptr, 0, false);
  }
  if (flags & kSetData) {
    live::SetQosListenData(session_id,
                           data ? static_cast<const uint8_t*>(static_cast<void*>(data)) : nullptr,
                           data_size, true);
  } else if (flags & kEnable) {
    live::SetQosListenData(session_id, nullptr, 0, true);
  }
  return 0;
}

// QoS probes for search results. RTT and the host's listen data come from the
// discovery reply that produced the result.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 num_xnaddr, u32 xnaddrs_ptr, u32 xnkids_ptr,
                               u32 xnkeys_ptr, u32 num_ina, u32 inas_ptr, u32 service_ids_ptr,
                               u32 probes, u32 bits_per_sec, u32 flags, u32 event_handle,
                               mapped_u32 pqos) {
  NET_TRACE();
  auto memory = REX_KERNEL_MEMORY();
  uint32_t total = num_xnaddr + num_ina;
  live::NetLog("XNetQosLookup(xnaddr={}, ina={})", num_xnaddr, num_ina);
  uint32_t size = 8 + std::max<uint32_t>(total, 1) * sizeof(XNQOSINFO);
  uint32_t qos_ptr = memory->SystemHeapAlloc(size);
  auto qos = memory->TranslateVirtual<XNQOS*>(qos_ptr);
  std::memset(qos, 0, size);
  qos->count = total;
  qos->count_pending = 0;

  constexpr uint8_t kComplete = 0x01, kContacted = 0x02, kDataReceived = 0x08;
  auto kid_ptrs = xnkids_ptr ? memory->TranslateVirtual<rex::be<uint32_t>*>(xnkids_ptr) : nullptr;

  for (uint32_t i = 0; i < total; i++) {
    auto& info = qos->info[i];
    info.flags = kComplete | kContacted;
    info.probes_xmit = 4;
    info.probes_recv = 4;
    info.rtt_min_in_msecs = 20;
    info.rtt_med_in_msecs = 25;
    info.up_bits_per_sec = 10000000;
    info.down_bits_per_sec = 10000000;

    if (i < num_xnaddr && kid_ptrs && kid_ptrs[i]) {
      auto kid = memory->TranslateVirtual<XNKID*>(kid_ptrs[i]);
      if (auto session = live::FindKnownSession(live::XnkidToU64(*kid))) {
        uint16_t rtt = static_cast<uint16_t>(std::min<uint32_t>(session->rtt_ms, 0xFFFF));
        info.rtt_min_in_msecs = rtt;
        info.rtt_med_in_msecs = rtt;
        if (!session->qos_data.empty()) {
          uint32_t data_ptr = memory->SystemHeapAlloc(static_cast<uint32_t>(session->qos_data.size()));
          std::memcpy(memory->TranslateVirtual(data_ptr), session->qos_data.data(),
                      session->qos_data.size());
          info.data_ptr = data_ptr;
          info.data_len = static_cast<uint16_t>(session->qos_data.size());
          info.flags |= kDataReceived;
        }
      }
    }
  }

  if (pqos) {
    *pqos = qos_ptr;
  }
  SignalEventHandle(event_handle);
  return 0;
}

u32 NetDll_XNetQosGetListenStats_entry(u32 caller, ppc_ptr_t<XNKID> xnkid, mapped_void stats) {
  NET_TRACE();
  return 0;
}

u32 XNetLogonGetMachineID_entry(mapped_u64 machine_id_ptr) {
  NET_TRACE();
  if (machine_id_ptr) {
    *machine_id_ptr = live::LocalMachineId();
  }
  return 0;
}

u32 XNetLogonGetTitleID_entry(u32 caller, mapped_void params) {
  NET_TRACE();
  return REX_KERNEL_STATE()->title_id();
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  NET_TRACE();
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  NET_TRACE();
  auto socket = object_ref<XSocket>(new XSocket(REX_KERNEL_STATE()));
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));
  live::NetLog("socket(af={}, type={}, proto={}) -> {} {:08X}", af, type, protocol,
               XSUCCEEDED(result), XSUCCEEDED(result) ? socket->handle() : 0);

  if (XFAILED(result)) {
    socket->ReleaseHandle();
    SetSocketError();
    return -1;
  }

  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  auto& t = socket->traffic();
  live::NetLog("closesocket {:08X} {}:{} (sent {} pkts, recv {} pkts)", socket_handle,
               socket->is_vdp() ? "vdp" : "ip", socket->bound_port(), t.sent_packets.load(),
               t.recv_packets.load());
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    SetSocketError();
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  if (XFAILED(status)) {
    SetSocketError();
    return -1;
  }
  return 0;
}

u32 NetDll_getsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, mapped_u32 optlen_ptr) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (!optval_ptr || !optlen_ptr) {
    XThread::SetLastError(kWsaEFault);
    return -1;
  }
  uint32_t len = *optlen_ptr;
  X_STATUS status = socket->GetOption(level, optname, optval_ptr, &len);
  if (XFAILED(status)) {
    SetSocketError();
    return -1;
  }
  *optlen_ptr = len;
  return 0;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    SetSocketError();
    return -1;
  }
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (!name) {
    XThread::SetLastError(kWsaEFault);
    return -1;
  }

  XSOCKADDR_IN addr = *name;
  if (addr.sin_addr == 0 && live::BindIp()) {
    addr.sin_addr = live::BindIp();
  }
  X_STATUS status = socket->Bind(&addr, namelen);
  if (XFAILED(status)) {
    SetSocketError();
    live::NetLog("bind {}:{} ({}) FAILED, error {}", IpString(addr.sin_addr),
                 addr.sin_port.get(), socket->is_vdp() ? "vdp" : "ip", XSocket::LastError());
    return -1;
  }
  live::NetLog("bind {}:{} ({})", IpString(addr.sin_addr), socket->bound_port(),
               socket->is_vdp() ? "vdp" : "ip");
  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                         u32 namelen) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  if (!name) {
    XThread::SetLastError(kWsaEFault);
    return -1;
  }

  EnsureImplicitBind(socket.get());
  XSOCKADDR_IN addr = *name;
  live::NetLog("connect({}:{})", IpString(addr.sin_addr), addr.sin_port.get());
  X_STATUS status = socket->Connect(&addr, namelen);
  if (XFAILED(status)) {
    SetSocketError();
    return -1;
  }
  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    SetSocketError();
    return -1;
  }
  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  XSOCKADDR_IN addr = {};
  uint32_t len = 0;
  auto new_socket = socket->Accept(&addr, &len);
  if (!new_socket) {
    SetSocketError();
    return -1;
  }
  if (addr_ptr) {
    *addr_ptr = addr;
  }
  if (addrlen_ptr) {
    *addrlen_ptr = len;
  }
  return new_socket->handle();
}

u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 namelen) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  XSOCKADDR_IN addr = {};
  if (!socket->GetSockName(&addr)) {
    SetSocketError();
    return -1;
  }
  if (name) {
    *name = addr;
  }
  if (namelen) {
    *namelen = sizeof(XSOCKADDR_IN);
  }
  return 0;
}

u32 NetDll_getpeername_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 namelen) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }
  XSOCKADDR_IN addr = {};
  if (!socket->GetPeerName(&addr)) {
    SetSocketError();
    return -1;
  }
  if (name) {
    *name = addr;
  }
  if (namelen) {
    *namelen = sizeof(XSOCKADDR_IN);
  }
  return 0;
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    this->count = std::min<uint32_t>(guest_set->fd_count, 64);
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      if (!socket) {
        this->count = i;
        break;
      }
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  NET_TRACE();
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int max_fd = 0;
#if !REX_PLATFORM_WIN32
  for (uint32_t i = 0; i < host_readfds.count; i++)
    max_fd = std::max<int>(max_fd, static_cast<int>(host_readfds.sockets[i]->native_handle()));
  for (uint32_t i = 0; i < host_writefds.count; i++)
    max_fd = std::max<int>(max_fd, static_cast<int>(host_writefds.sockets[i]->native_handle()));
  for (uint32_t i = 0; i < host_exceptfds.count; i++)
    max_fd = std::max<int>(max_fd, static_cast<int>(host_exceptfds.sockets[i]->native_handle()));
  max_fd += 1;
#endif
  int ret = select(max_fd, readfds ? &native_readfds : nullptr,
                   writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (ret < 0) {
    SetSocketError();
    return ret;
  }
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  int ret = socket->Recv(buf_ptr, buf_len, flags);
  if (ret < 0) {
    SetSocketError();
  }
  return ret;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  XSOCKADDR_IN from = {};
  uint32_t from_len = 0;
  int ret = socket->RecvFrom(buf_ptr, buf_len, flags, &from, &from_len);
  if (ret < 0) {
    SetSocketError();
    return ret;
  }
  if (from_ptr) {
    *from_ptr = from;
  }
  if (fromlen_ptr) {
    *fromlen_ptr = from_len;
  }
  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  int ret = socket->Send(buf_ptr, buf_len, flags);
  if (ret < 0) {
    SetSocketError();
  }
  return ret;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  NET_TRACE();
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    return -1;
  }

  EnsureImplicitBind(socket.get());
  const XSOCKADDR_IN* to = to_ptr ? static_cast<const XSOCKADDR_IN*>(to_ptr) : nullptr;
  int ret = socket->SendTo(buf_ptr, buf_len, flags, to);
  if (ret < 0) {
    SetSocketError();
  }
  return ret;
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  NET_TRACE();
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  NET_TRACE();
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSARecv, rex::kernel::xam::NetDll_WSARecv_entry)
REX_EXPORT(__imp__NetDll_WSAGetOverlappedResult,
           rex::kernel::xam::NetDll_WSAGetOverlappedResult_entry)
REX_EXPORT(__imp__NetDll_WSACancelOverlappedIO,
           rex::kernel::xam::NetDll_WSACancelOverlappedIO_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus,
           rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterInAddr,
           rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetServerToInAddr, rex::kernel::xam::NetDll_XNetServerToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosGetListenStats,
           rex::kernel::xam::NetDll_XNetQosGetListenStats_entry)
REX_EXPORT(__imp__XNetLogonGetMachineID, rex::kernel::xam::XNetLogonGetMachineID_entry)
REX_EXPORT(__imp__XNetLogonGetTitleID, rex::kernel::xam::XNetLogonGetTitleID_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_getsockopt, rex::kernel::xam::NetDll_getsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
REX_EXPORT(__imp__NetDll_getpeername, rex::kernel::xam::NetDll_getpeername_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_EXPORT_STUB(__imp__NetDll_WSASend);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
