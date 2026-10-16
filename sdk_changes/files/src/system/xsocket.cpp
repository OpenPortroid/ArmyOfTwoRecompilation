/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - Real socket I/O for peer-to-peer play, based on the
 *              Xenia Canary netplay fork (AdrianCassar/xenia-canary)
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <rex/cvar.h>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>

#include <rex/net/socket.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_INT32(net_debug_delay_ms, 0, "Network",
                     "Testing: hold every outgoing datagram (sendto) this long, as a slow link would");
REXCVAR_DEFINE_INT32(net_debug_jitter_ms, 0, "Network",
                     "Testing: hold every outgoing datagram a random 0..N ms longer (uneven arrival, reordering)");

namespace rex::system {

namespace {

// net_debug_delay_ms / net_debug_jitter_ms: datagrams wait here for their time.
class DelayedSender {
 public:
  using clock = std::chrono::steady_clock;

  void Send(uint64_t native_handle, const uint8_t* buf, uint32_t len, int flags, const sockaddr_in& addr,
            int delay_ms) {
    std::lock_guard lock(mutex_);
    if (!started_) {
      started_ = true;
      std::thread([this] { Run(); }).detach();
    }
    queue_.emplace(clock::now() + std::chrono::milliseconds(delay_ms),
                   Packet{native_handle, std::vector<char>(buf, buf + len), flags, addr});
    wake_.notify_one();
  }

  int Delay() {
    int delay = std::max(REXCVAR_GET(net_debug_delay_ms), 0);
    int jitter = std::max(REXCVAR_GET(net_debug_jitter_ms), 0);
    if (jitter > 0) {
      std::lock_guard lock(mutex_);
      delay += int(random_() % uint32_t(jitter + 1));
    }
    return delay;
  }

 private:
  struct Packet {
    uint64_t native_handle;
    std::vector<char> data;
    int flags;
    sockaddr_in addr;
  };

  void Run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      if (queue_.empty()) {
        wake_.wait(lock);
        continue;
      }
      auto due = queue_.begin()->first;
      if (clock::now() < due) {
        wake_.wait_until(lock, due);
        continue;
      }
      Packet packet = std::move(queue_.begin()->second);
      queue_.erase(queue_.begin());
      lock.unlock();
      sendto(packet.native_handle, packet.data.data(), int(packet.data.size()), packet.flags,
             reinterpret_cast<sockaddr*>(&packet.addr), sizeof(packet.addr));
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::multimap<clock::time_point, Packet> queue_;
  std::minstd_rand random_{12345};
  bool started_ = false;
};

DelayedSender g_delayed_sender;

constexpr uint32_t kWsaIoPending = 997;
constexpr uint32_t kWsaIoIncomplete = 996;
constexpr uint32_t kWsaOperationAborted = 995;
constexpr uint32_t kStatusPending = 0x00000103;

sockaddr_in ToHost(const XSOCKADDR_IN* in) {
  sockaddr_in out = {};
  out.sin_family = AF_INET;
  if (in) {
    out.sin_port = htons(in->sin_port);
    out.sin_addr.s_addr = in->sin_addr;
  }
  return out;
}

void ToGuest(const sockaddr_in& in, XSOCKADDR_IN* out) {
  if (!out) {
    return;
  }
  out->sin_family = static_cast<uint16_t>(in.sin_family);
  out->sin_port = ntohs(in.sin_port);
  out->sin_addr = in.sin_addr.s_addr;
  std::memset(out->x_sin_zero, 0, sizeof(out->x_sin_zero));
}

// Waits until the socket is readable. Returns >0 readable, 0 timeout, <0 error.
int WaitReadable(uint64_t handle, uint32_t timeout_ms) {
  fd_set readfds;
  FD_ZERO(&readfds);
#if REX_PLATFORM_WIN32
  FD_SET(static_cast<SOCKET>(handle), &readfds);
  int nfds = 0;
#else
  FD_SET(static_cast<int>(handle), &readfds);
  int nfds = static_cast<int>(handle) + 1;
#endif
  timeval tv = {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return select(nfds, &readfds, nullptr, nullptr, &tv);
}

void SignalEvent(KernelState* kernel_state, uint32_t handle) {
  if (!handle) {
    return;
  }
  auto ev = kernel_state->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    ev->Set(0, false);
  }
}

void ResetEvent(KernelState* kernel_state, uint32_t handle) {
  if (!handle) {
    return;
  }
  auto ev = kernel_state->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    ev->Reset();
  }
}

}  // namespace

uint32_t XSocket::LastError() {
#if REX_PLATFORM_WIN32
  return static_cast<uint32_t>(WSAGetLastError());
#else
  switch (errno) {
    case EWOULDBLOCK:
      return 10035;  // WSAEWOULDBLOCK
    case EINPROGRESS:
      return 10036;
    case EALREADY:
      return 10037;
    case ENOTSOCK:
      return 10038;
    case EMSGSIZE:
      return 10040;
    case EADDRINUSE:
      return 10048;
    case EADDRNOTAVAIL:
      return 10049;
    case ENETUNREACH:
      return 10051;
    case ECONNABORTED:
      return 10053;
    case ECONNRESET:
      return 10054;
    case EISCONN:
      return 10056;
    case ENOTCONN:
      return 10057;
    case ETIMEDOUT:
      return 10060;
    case ECONNREFUSED:
      return 10061;
    case EHOSTUNREACH:
      return 10065;
    case EINVAL:
      return 10022;
    default:
      return 10022;
  }
#endif
}

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;
  vdp_ = false;

  if (!type) {
    type_ = (proto == X_IPPROTO_TCP) ? X_SOCK_STREAM : X_SOCK_DGRAM;
  }
  if (!proto) {
    proto_ = (type_ == X_SOCK_STREAM) ? X_IPPROTO_TCP : X_IPPROTO_UDP;
  } else if (proto == X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto_ = X_IPPROTO_UDP;
    vdp_ = true;
  }

  native_handle_ = socket(af, type_, proto_);
  if (native_handle_ == uint64_t(-1)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  CancelOverlapped();
  if (native_handle_ == uint64_t(-1)) {
    return X_STATUS_SUCCESS;
  }

  int ret = rex::net::socket_close(native_handle_);
  native_handle_ = uint64_t(-1);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  // Xbox-specific SOL_SOCKET options (encryption, VDP voice, etc.) have no
  // host equivalent. Accept them.
  if (level == 0xFFFF && optname >= 0x5801 && optname <= 0x5810) {
    return X_STATUS_SUCCESS;
  }

  // Guest integers are big-endian.
  uint8_t value[16] = {};
  optlen = std::min<uint32_t>(optlen, sizeof(value));
  std::memcpy(value, optval_ptr, optlen);
  if (optlen == 4) {
    uint32_t v;
    std::memcpy(&v, value, 4);
    v = rex::byte_swap(v);
    std::memcpy(value, &v, 4);
  } else if (optlen == 2) {
    uint16_t v;
    std::memcpy(&v, value, 2);
    v = rex::byte_swap(v);
    std::memcpy(value, &v, 2);
  }

  // Guest level/option numbers match WinSock. Translate for POSIX hosts.
  int host_level = static_cast<int>(level);
  int host_opt = static_cast<int>(optname);
#if !REX_PLATFORM_WIN32
  if (level == 0xFFFF) {
    host_level = SOL_SOCKET;
    switch (optname) {
      case 0x0004:
        host_opt = SO_REUSEADDR;
        break;
      case 0x0020:
        host_opt = SO_BROADCAST;
        break;
      case 0x1001:
        host_opt = SO_SNDBUF;
        break;
      case 0x1002:
        host_opt = SO_RCVBUF;
        break;
      case 0x0080:
        host_opt = SO_LINGER;
        break;
      default:
        break;
    }
  }
#endif

  int ret = setsockopt(native_handle_, host_level, host_opt, reinterpret_cast<char*>(value),
                       static_cast<socklen_t>(optlen));
  if (ret < 0) {
    REXKRNL_WARN("XSocket::SetOption({:X}, {:X}) failed: {}", level, optname, LastError());
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t* optlen) {
  uint8_t value[16] = {};
  socklen_t len = static_cast<socklen_t>(std::min<uint32_t>(*optlen, sizeof(value)));
  int ret = getsockopt(native_handle_, level, optname, reinterpret_cast<char*>(value), &len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  if (len == 4) {
    uint32_t v;
    std::memcpy(&v, value, 4);
    v = rex::byte_swap(v);
    std::memcpy(value, &v, 4);
  } else if (len == 2) {
    uint16_t v;
    std::memcpy(&v, value, 2);
    v = rex::byte_swap(v);
    std::memcpy(value, &v, 2);
  }
  std::memcpy(optval_ptr, value, len);
  *optlen = static_cast<uint32_t>(len);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  // Guest passes a big-endian u_long.
  uint32_t arg = rex::byte_swap(*reinterpret_cast<uint32_t*>(arg_ptr));
  int ret = rex::net::socket_ioctl(native_handle_, cmd, reinterpret_cast<uint8_t*>(&arg));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  *reinterpret_cast<uint32_t*>(arg_ptr) = rex::byte_swap(arg);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(const XSOCKADDR_IN* name, int name_len) {
  sockaddr_in addr = ToHost(name);
  int ret = connect(native_handle_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(const XSOCKADDR_IN* name, int name_len) {
  sockaddr_in addr = ToHost(name);
  int ret = bind(native_handle_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (ret < 0) {
    REXKRNL_WARN("XSocket::Bind({:08X}:{}) failed: {}", ntohl(addr.sin_addr.s_addr),
                 ntohs(addr.sin_port), LastError());
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  XSOCKADDR_IN actual = {};
  if (GetSockName(&actual)) {
    bound_port_ = actual.sin_port;
  } else {
    bound_port_ = name->sin_port;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(XSOCKADDR_IN* name, uint32_t* name_len) {
  sockaddr_in addr = {};
  socklen_t addr_len = sizeof(addr);
  auto ret = accept(native_handle_, reinterpret_cast<sockaddr*>(&addr), &addr_len);
  if (static_cast<uint64_t>(ret) == uint64_t(-1)) {
    return nullptr;
  }

  if (name) {
    ToGuest(addr, name);
  }
  if (name_len) {
    *name_len = sizeof(XSOCKADDR_IN);
  }

  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, static_cast<uint64_t>(ret)));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;
  socket->vdp_ = vdp_;
  socket->bound_ = true;
  socket->bound_port_ = bound_port_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

bool XSocket::GetSockName(XSOCKADDR_IN* name) {
  sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  if (getsockname(native_handle_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return false;
  }
  ToGuest(addr, name);
  return true;
}

bool XSocket::GetPeerName(XSOCKADDR_IN* name) {
  sockaddr_in addr = {};
  socklen_t len = sizeof(addr);
  if (getpeername(native_handle_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return false;
  }
  ToGuest(addr, name);
  return true;
}

namespace {
void CountSend(XSocket::Traffic& t, int ret, const sockaddr_in* to) {
  if (ret < 0) {
    t.send_errors++;
    return;
  }
  t.sent_packets++;
  t.sent_bytes += static_cast<uint64_t>(ret);
  if (to) {
    t.last_to_ip = to->sin_addr.s_addr;
    t.last_to_port = ntohs(to->sin_port);
  }
}

void CountRecv(XSocket::Traffic& t, int ret, const sockaddr_in* from) {
  if (ret < 0) {
    // WSAEWOULDBLOCK on a non-blocking socket is not an error.
    if (XSocket::LastError() != 10035) t.recv_errors++;
    return;
  }
  t.recv_packets++;
  t.recv_bytes += static_cast<uint64_t>(ret);
  if (from) {
    t.last_from_ip = from->sin_addr.s_addr;
    t.last_from_port = ntohs(from->sin_port);
  }
}
}  // namespace

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  int ret = recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
  CountRecv(traffic_, ret, nullptr);
  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  int ret = send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
  CountSend(traffic_, ret, nullptr);
  return ret;
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  sockaddr_in addr = {};
  socklen_t addr_len = sizeof(addr);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     reinterpret_cast<sockaddr*>(&addr), &addr_len);
  CountRecv(traffic_, ret, proto_ == X_IPPROTO_TCP ? nullptr : &addr);
  if (ret >= 0) {
    if (proto_ == X_IPPROTO_TCP) {
      socklen_t peer_len = sizeof(addr);
      getpeername(native_handle_, reinterpret_cast<sockaddr*>(&addr), &peer_len);
    }
    ToGuest(addr, from);
    if (from_len) {
      *from_len = sizeof(XSOCKADDR_IN);
    }
  }
  return ret;
}

int XSocket::SendTo(const uint8_t* buf, uint32_t buf_len, uint32_t flags,
                    const XSOCKADDR_IN* to) {
  if (!to) {
    return Send(buf, buf_len, flags);
  }
  sockaddr_in addr = ToHost(to);
  if (int delay = g_delayed_sender.Delay(); delay > 0) {
    g_delayed_sender.Send(uint64_t(native_handle_), buf, buf_len, int(flags), addr, delay);
    CountSend(traffic_, int(buf_len), &addr);
    return int(buf_len);
  }
  int ret = sendto(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags,
                   reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  CountSend(traffic_, ret, &addr);
  return ret;
}

int XSocket::ReceiveInto(uint32_t buffers_ptr, uint32_t num_buffers, uint32_t from_ptr,
                         uint32_t fromlen_ptr) {
  auto memory = kernel_state_->memory();
  auto buffers = memory->TranslateVirtual<XWSABUF*>(buffers_ptr);

  uint32_t total = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    total += buffers[i].len;
  }

  std::vector<uint8_t> temp(std::max<uint32_t>(total, 1));
  XSOCKADDR_IN from = {};
  int ret = RecvFrom(temp.data(), total, 0, &from, nullptr);
  if (ret < 0) {
    return ret;
  }

  uint32_t offset = 0;
  for (uint32_t i = 0; i < num_buffers && offset < static_cast<uint32_t>(ret); i++) {
    uint32_t chunk = std::min<uint32_t>(buffers[i].len, static_cast<uint32_t>(ret) - offset);
    std::memcpy(memory->TranslateVirtual(buffers[i].buf_ptr), temp.data() + offset, chunk);
    offset += chunk;
  }

  if (from_ptr) {
    *memory->TranslateVirtual<XSOCKADDR_IN*>(from_ptr) = from;
  }
  if (fromlen_ptr) {
    *memory->TranslateVirtual<rex::be<uint32_t>*>(fromlen_ptr) = sizeof(XSOCKADDR_IN);
  }
  return ret;
}

int XSocket::WSARecvFrom(uint32_t buffers_ptr, uint32_t num_buffers, uint32_t bytes_ptr,
                         uint32_t flags_ptr, uint32_t from_ptr, uint32_t fromlen_ptr,
                         uint32_t overlapped_ptr) {
  auto memory = kernel_state_->memory();
  auto overlapped =
      overlapped_ptr ? memory->TranslateVirtual<XWSAOVERLAPPED*>(overlapped_ptr) : nullptr;

  std::unique_lock<std::mutex> lock(io_mutex_);

  // Immediate completion when data is already waiting (or for blocking,
  // non-overlapped calls).
  if (!overlapped || WaitReadable(native_handle_, 0) > 0) {
    int ret = ReceiveInto(buffers_ptr, num_buffers, from_ptr, fromlen_ptr);
    if (ret < 0) {
      uint32_t error = LastError();
      XThread::SetLastError(error);
      if (overlapped) {
        overlapped->internal = error;
      }
      return -1;
    }
    if (bytes_ptr) {
      *memory->TranslateVirtual<rex::be<uint32_t>*>(bytes_ptr) = static_cast<uint32_t>(ret);
    }
    if (flags_ptr) {
      *memory->TranslateVirtual<rex::be<uint32_t>*>(flags_ptr) = 0;
    }
    if (overlapped) {
      overlapped->internal = 0;
      overlapped->internal_high = static_cast<uint32_t>(ret);
      overlapped->offset = 0;
      SignalEvent(kernel_state_, overlapped->event_handle);
    }
    return 0;
  }

  // Deferred completion.
  overlapped->internal = kStatusPending;
  overlapped->internal_high = 0;
  ResetEvent(kernel_state_, overlapped->event_handle);

  Retain();
  std::thread(&XSocket::PendingReceiveWorker, this, buffers_ptr, num_buffers, from_ptr,
              fromlen_ptr, overlapped_ptr)
      .detach();

  XThread::SetLastError(kWsaIoPending);
  return -1;
}

void XSocket::PendingReceiveWorker(uint32_t buffers_ptr, uint32_t num_buffers, uint32_t from_ptr,
                                   uint32_t fromlen_ptr, uint32_t overlapped_ptr) {
  auto memory = kernel_state_->memory();
  auto overlapped = memory->TranslateVirtual<XWSAOVERLAPPED*>(overlapped_ptr);

  while (!cancel_ && native_handle_ != uint64_t(-1)) {
    int ready = WaitReadable(native_handle_, 100);
    if (ready == 0) {
      continue;
    }
    std::unique_lock<std::mutex> lock(io_mutex_);
    if (cancel_ || native_handle_ == uint64_t(-1)) {
      break;
    }
    if (ready < 0) {
      overlapped->internal = LastError();
      SignalEvent(kernel_state_, overlapped->event_handle);
      Release();
      return;
    }
    int ret = ReceiveInto(buffers_ptr, num_buffers, from_ptr, fromlen_ptr);
    if (ret < 0) {
      uint32_t error = LastError();
      if (error == 10035) {  // raced with another reader
        continue;
      }
      overlapped->internal = error;
    } else {
      overlapped->internal_high = static_cast<uint32_t>(ret);
      overlapped->offset = 0;
      overlapped->internal = 0;
    }
    SignalEvent(kernel_state_, overlapped->event_handle);
    Release();
    return;
  }

  overlapped->internal = kWsaOperationAborted;
  SignalEvent(kernel_state_, overlapped->event_handle);
  Release();
}

bool XSocket::WSAGetOverlappedResult(uint32_t overlapped_ptr, uint32_t bytes_ptr, bool wait,
                                     uint32_t flags_ptr) {
  auto memory = kernel_state_->memory();
  auto overlapped = memory->TranslateVirtual<XWSAOVERLAPPED*>(overlapped_ptr);

  if (wait) {
    while (overlapped->internal == kStatusPending && !cancel_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  uint32_t status = overlapped->internal;
  if (status == kStatusPending) {
    XThread::SetLastError(kWsaIoIncomplete);
    return false;
  }
  if (bytes_ptr) {
    *memory->TranslateVirtual<rex::be<uint32_t>*>(bytes_ptr) =
        static_cast<uint32_t>(overlapped->internal_high);
  }
  if (flags_ptr) {
    *memory->TranslateVirtual<rex::be<uint32_t>*>(flags_ptr) =
        static_cast<uint32_t>(overlapped->offset);
  }
  if (status != 0) {
    XThread::SetLastError(status);
    return false;
  }
  return true;
}

void XSocket::CancelOverlapped() {
  cancel_ = true;
}

}  // namespace rex::system
