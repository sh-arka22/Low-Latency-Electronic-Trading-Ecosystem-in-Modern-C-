#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/logging.h"
#include "common/macros.h"
#include "common/tcp_socket.h"
#include "common/time_utils.h"

namespace Common {
  /// Accepts incoming TCP connections on a listening socket; fans out poll()-
  /// driven recv/send across all client sockets. POSIX poll() (not epoll) so
  /// it builds on macOS *and* Linux.
  struct TCPServer {
    explicit TCPServer(Logger &logger) : listener_socket_(logger), logger_(logger) {}

    auto listen(const std::string &iface, int port) -> void;

    /// Accept any pending connections; drop dead ones. Non-blocking.
    auto poll() noexcept -> void;

    /// Run sendAndRecv on every connected socket. After all sockets have been
    /// drained, invoke recv_finished_callback_ once.
    auto sendAndRecv() noexcept -> void;

    /// Helper used by sendAndRecv to dispatch RX bytes to higher-level handler.
    auto defaultRecvCallback(TCPSocket *socket, Nanos rx_time) noexcept -> void {
      if (recv_callback_) recv_callback_(socket, rx_time);
    }

    TCPServer() = delete;
    TCPServer(const TCPServer &) = delete;
    TCPServer(const TCPServer &&) = delete;
    TCPServer &operator=(const TCPServer &) = delete;
    TCPServer &operator=(const TCPServer &&) = delete;

    int                     efd_ = -1;  // unused on macOS; reserved for Linux epoll port
    TCPSocket               listener_socket_;
    std::vector<TCPSocket*> receive_sockets_;
    std::vector<TCPSocket*> send_sockets_;

    std::function<void(TCPSocket *, Nanos rx_time)> recv_callback_         = nullptr;
    std::function<void()>                            recv_finished_callback_ = nullptr;

    std::string time_str_;
    Logger     &logger_;
  };
}
