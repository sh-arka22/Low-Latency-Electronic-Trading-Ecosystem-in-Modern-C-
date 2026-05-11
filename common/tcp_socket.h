#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <netinet/in.h>
#include <string>
#include <vector>

#include "common/logging.h"
#include "common/macros.h"
#include "common/socket_utils.h"
#include "common/time_utils.h"

namespace Common {
  constexpr size_t TCPBufferSize = 64 * 1024 * 1024;

  /// One TCP connection. Owns a 64 MiB send + 64 MiB recv buffer so the caller
  /// can batch writes/reads without per-message syscalls.
  struct TCPSocket {
    explicit TCPSocket(Logger &logger) : logger_(logger) {
      outbound_data_.resize(TCPBufferSize);
      inbound_data_.resize(TCPBufferSize);
    }

    /// Either connect to (is_listening=false) or listen on (is_listening=true)
    /// the given iface/ip/port. Returns fd.
    auto connect(const std::string &ip, const std::string &iface, int port,
                 bool is_listening) -> int;

    /// Push pending writes; read whatever is available and invoke recv_callback_.
    /// Returns true if any data was received this call.
    auto sendAndRecv() noexcept -> bool;

    /// Append bytes to the outbound buffer (not flushed until sendAndRecv()).
    auto send(const void *data, size_t len) noexcept -> void;

    TCPSocket() = delete;
    TCPSocket(const TCPSocket &) = delete;
    TCPSocket(const TCPSocket &&) = delete;
    TCPSocket &operator=(const TCPSocket &) = delete;
    TCPSocket &operator=(const TCPSocket &&) = delete;

    int socket_fd_ = -1;

    std::vector<char> outbound_data_;
    size_t            next_send_valid_index_ = 0;
    std::vector<char> inbound_data_;
    size_t            next_rcv_valid_index_  = 0;

    struct sockaddr_in socket_attrib_{};

    /// Called once per sendAndRecv() round if new bytes arrived.
    std::function<void(TCPSocket *s, Nanos rx_time)> recv_callback_ = nullptr;

    std::string time_str_;
    Logger     &logger_;
  };
}
