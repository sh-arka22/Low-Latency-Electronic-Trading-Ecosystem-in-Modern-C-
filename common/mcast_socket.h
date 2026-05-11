#pragma once

#include <cstddef>
#include <functional>
#include <netinet/in.h>
#include <string>
#include <vector>

#include "common/logging.h"
#include "common/macros.h"
#include "common/socket_utils.h"
#include "common/time_utils.h"

namespace Common {
  constexpr size_t McastBufferSize = 64 * 1024 * 1024;

  /// UDP multicast send/receive socket. Same buffered-batch model as TCPSocket:
  /// callers stage bytes with send(), the loop flushes once per round via
  /// sendAndRecv().
  struct McastSocket {
    explicit McastSocket(Logger &logger) : logger_(logger) {
      outbound_data_.resize(McastBufferSize);
      inbound_data_.resize(McastBufferSize);
    }

    /// Create + bind/connect the socket. Returns fd, or aborts on failure.
    auto init(const std::string &ip, const std::string &iface, int port,
              bool is_listening) -> int;

    /// Join a multicast group (subscriber side only).
    auto join(const std::string &ip) -> bool;

    /// Append bytes to outbound buffer.
    auto send(const void *data, size_t len) noexcept -> void;

    /// Flush outbound bytes via ::send(). On the RX side, drain available
    /// datagrams and invoke recv_callback_.
    auto sendAndRecv() noexcept -> bool;

    McastSocket() = delete;
    McastSocket(const McastSocket &) = delete;
    McastSocket(const McastSocket &&) = delete;
    McastSocket &operator=(const McastSocket &) = delete;
    McastSocket &operator=(const McastSocket &&) = delete;

    int               socket_fd_ = -1;
    std::vector<char> outbound_data_;
    size_t            next_send_valid_index_ = 0;
    std::vector<char> inbound_data_;
    size_t            next_rcv_valid_index_  = 0;

    std::function<void(McastSocket *)> recv_callback_ = nullptr;

    std::string time_str_;
    Logger     &logger_;
  };
}
