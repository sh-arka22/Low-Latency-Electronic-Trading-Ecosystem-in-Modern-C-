#include "common/tcp_socket.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "common/time_utils.h"

namespace Common {
  auto TCPSocket::connect(const std::string &ip, const std::string &iface,
                          int port, bool is_listening) -> int {
    const SocketCfg cfg{ip, iface, port, /*is_udp*/ false, is_listening,
                        /*needs_so_timestamp*/ true};
    socket_fd_ = createSocket(logger_, cfg);

    socket_attrib_.sin_addr.s_addr = INADDR_ANY;
    socket_attrib_.sin_port        = htons(static_cast<uint16_t>(port));
    socket_attrib_.sin_family      = AF_INET;
    return socket_fd_;
  }

  auto TCPSocket::send(const void *data, size_t len) noexcept -> void {
    if (UNLIKELY(next_send_valid_index_ + len > outbound_data_.size())) {
      FATAL("TCPSocket outbound buffer overflow");
    }
    std::memcpy(outbound_data_.data() + next_send_valid_index_, data, len);
    next_send_valid_index_ += len;
  }

  /// Drain any pending writes (one or more send() syscalls), then read
  /// whatever is available. If new bytes arrived, fire recv_callback_.
  auto TCPSocket::sendAndRecv() noexcept -> bool {
    // --- read ---
    ssize_t n_rcv = 0;
    if (next_rcv_valid_index_ < inbound_data_.size()) {
      n_rcv = ::recv(socket_fd_,
                     inbound_data_.data() + next_rcv_valid_index_,
                     inbound_data_.size() - next_rcv_valid_index_,
                     0);
      if (n_rcv > 0) {
        next_rcv_valid_index_ += static_cast<size_t>(n_rcv);
        std::string time_str;
        logger_.log("%:% %() % read socket:% len:% rcvd:%\n", __FILE__,
                    __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str),
                    socket_fd_, next_rcv_valid_index_, n_rcv);
        if (recv_callback_) recv_callback_(this, getCurrentNanos());
      }
    }

    // --- write ---
    if (next_send_valid_index_ > 0) {
      const ssize_t n =
          ::send(socket_fd_, outbound_data_.data(), next_send_valid_index_,
#if defined(MSG_NOSIGNAL)
                 MSG_NOSIGNAL
#else
                 0
#endif
          );
      if (n > 0) {
        // Shift any unsent leftover to the front of the buffer.
        if (static_cast<size_t>(n) < next_send_valid_index_) {
          std::memmove(outbound_data_.data(),
                       outbound_data_.data() + n,
                       next_send_valid_index_ - static_cast<size_t>(n));
        }
        next_send_valid_index_ -= static_cast<size_t>(n);
      }
    }
    return n_rcv > 0;
  }
}
