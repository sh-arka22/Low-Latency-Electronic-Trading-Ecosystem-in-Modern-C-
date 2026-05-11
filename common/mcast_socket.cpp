#include "common/mcast_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace Common {
  auto McastSocket::init(const std::string &ip, const std::string &iface,
                         int port, bool is_listening) -> int {
    const SocketCfg cfg{ip, iface, port, /*is_udp*/ true, is_listening,
                        /*needs_so_timestamp*/ false};
    socket_fd_ = createSocket(logger_, cfg);
    return socket_fd_;
  }

  auto McastSocket::join(const std::string &ip) -> bool {
    return Common::join(socket_fd_, ip);
  }

  auto McastSocket::send(const void *data, size_t len) noexcept -> void {
    if (UNLIKELY(next_send_valid_index_ + len > outbound_data_.size())) {
      FATAL("McastSocket outbound buffer overflow");
    }
    std::memcpy(outbound_data_.data() + next_send_valid_index_, data, len);
    next_send_valid_index_ += len;
  }

  /// On the publisher side this flushes outbound_data_ through ::send().
  /// On the subscriber side it drains datagrams and calls recv_callback_.
  auto McastSocket::sendAndRecv() noexcept -> bool {
    bool got_data = false;

    // RX path.
    while (true) {
      const ssize_t n = ::recv(socket_fd_,
                               inbound_data_.data() + next_rcv_valid_index_,
                               inbound_data_.size() - next_rcv_valid_index_,
                               0);
      if (n <= 0) break;
      next_rcv_valid_index_ += static_cast<size_t>(n);
      got_data = true;
    }
    if (got_data && recv_callback_) recv_callback_(this);

    // TX path: write whatever is buffered. UDP send is atomic per call; we
    // just push everything in one shot for now.
    if (next_send_valid_index_ > 0) {
      const ssize_t n = ::send(socket_fd_, outbound_data_.data(),
                               next_send_valid_index_,
#if defined(MSG_NOSIGNAL)
                               MSG_NOSIGNAL
#else
                               0
#endif
      );
      if (n > 0) {
        if (static_cast<size_t>(n) < next_send_valid_index_) {
          std::memmove(outbound_data_.data(),
                       outbound_data_.data() + n,
                       next_send_valid_index_ - static_cast<size_t>(n));
        }
        next_send_valid_index_ -= static_cast<size_t>(n);
      } else {
        // Drop on error to avoid blocking the publisher; UDP is best-effort.
        next_send_valid_index_ = 0;
      }
    }
    return got_data;
  }
}
