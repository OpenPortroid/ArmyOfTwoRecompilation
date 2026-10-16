#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - Real socket I/O for peer-to-peer play (byte order fixes,
 *              VDP, overlapped receive), based on the Xenia Canary netplay fork
 */

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/math.h>
#include <rex/system/xobject.h>
#include <rex/types.h>

namespace rex::system {

struct XSOCKADDR {
  rex::be<uint16_t> address_family;
  char sa_data[14];
};

// Guest sockaddr_in. sin_addr is stored exactly as the guest wrote it, which
// is network byte order (the same byte sequence the host socket API wants).
struct XSOCKADDR_IN {
  rex::be<uint16_t> sin_family;
  rex::be<uint16_t> sin_port;
  uint32_t sin_addr;  // network byte order
  char x_sin_zero[8];
};
static_assert(sizeof(XSOCKADDR_IN) == 16);

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  rex::be<uint32_t> offset;
  rex::be<uint32_t> offset_high;
  rex::be<uint32_t> event_handle;
};
static_assert(sizeof(XWSAOVERLAPPED) == 0x14);

class XSocket : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  enum AddressFamily {
    X_AF_INET = 2,
  };

  enum Type {
    X_SOCK_STREAM = 1,
    X_SOCK_DGRAM = 2,
  };

  enum Protocol {
    X_IPPROTO_TCP = 6,
    X_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol. Between two ReXGlue peers the payload is
    // passed through unchanged over UDP.
    X_IPPROTO_VDP = 254,
  };

  XSocket(KernelState* kernel_state);
  ~XSocket();

  uint64_t native_handle() const { return native_handle_; }
  uint16_t bound_port() const { return bound_port_; }
  bool bound() const { return bound_; }
  bool is_vdp() const { return vdp_; }

  X_STATUS Initialize(AddressFamily af, Type type, Protocol proto);
  X_STATUS Close();

  X_STATUS SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen);
  X_STATUS GetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t* optlen);
  X_STATUS IOControl(uint32_t cmd, uint8_t* arg_ptr);

  X_STATUS Connect(const XSOCKADDR_IN* name, int name_len);
  X_STATUS Bind(const XSOCKADDR_IN* name, int name_len);
  X_STATUS Listen(int backlog);
  object_ref<XSocket> Accept(XSOCKADDR_IN* name, uint32_t* name_len);
  int Shutdown(int how);
  bool GetSockName(XSOCKADDR_IN* name);
  bool GetPeerName(XSOCKADDR_IN* name);

  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags);

  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, XSOCKADDR_IN* from,
               uint32_t* from_len);
  int SendTo(const uint8_t* buf, uint32_t buf_len, uint32_t flags, const XSOCKADDR_IN* to);

  // Overlapped receive. Returns 0 on immediate completion, or -1 with the
  // last error set to WSA_IO_PENDING when completion is deferred.
  int WSARecvFrom(uint32_t buffers_ptr, uint32_t num_buffers, uint32_t bytes_ptr,
                  uint32_t flags_ptr, uint32_t from_ptr, uint32_t fromlen_ptr,
                  uint32_t overlapped_ptr);
  bool WSAGetOverlappedResult(uint32_t overlapped_ptr, uint32_t bytes_ptr, bool wait,
                              uint32_t flags_ptr);
  void CancelOverlapped();

  // Last native socket error translated to a guest WSA error code.
  static uint32_t LastError();

  // Packet counters for network diagnostics (net.log traffic summaries).
  struct Traffic {
    std::atomic<uint64_t> sent_packets{0}, sent_bytes{0}, send_errors{0};
    std::atomic<uint64_t> recv_packets{0}, recv_bytes{0}, recv_errors{0};
    std::atomic<uint32_t> last_to_ip{0}, last_from_ip{0};  // network order
    std::atomic<uint16_t> last_to_port{0}, last_from_port{0};
    uint64_t reported_sent = 0, reported_recv = 0, reported_errors = 0;  // reporter only
  };
  Traffic& traffic() { return traffic_; }

 private:
  XSocket(KernelState* kernel_state, uint64_t native_handle);

  // Receive into the guest scatter buffers. Returns bytes or -1.
  int ReceiveInto(uint32_t buffers_ptr, uint32_t num_buffers, uint32_t from_ptr,
                  uint32_t fromlen_ptr);
  void PendingReceiveWorker(uint32_t buffers_ptr, uint32_t num_buffers, uint32_t from_ptr,
                            uint32_t fromlen_ptr, uint32_t overlapped_ptr);

  uint64_t native_handle_ = uint64_t(-1);

  AddressFamily af_ = X_AF_INET;
  Type type_ = X_SOCK_DGRAM;
  Protocol proto_ = X_IPPROTO_UDP;
  bool vdp_ = false;

  bool bound_ = false;
  uint16_t bound_port_ = 0;
  bool broadcast_socket_ = false;

  std::mutex io_mutex_;
  std::atomic<bool> cancel_{false};
  Traffic traffic_;
  std::vector<std::thread> pending_;
};

}  // namespace rex::system
