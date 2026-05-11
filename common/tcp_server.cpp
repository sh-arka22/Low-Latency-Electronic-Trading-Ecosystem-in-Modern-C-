#include "common/tcp_server.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Common {
  /// Bind + listen on the given interface/port.
  auto TCPServer::listen(const std::string &iface, int port) -> void {
    listener_socket_.connect("", iface, port, /*is_listening*/ true);
    std::string time_str;
    logger_.log("%:% %() % listening on iface:% port:%\n", __FILE__, __LINE__,
                __FUNCTION__, getCurrentTimeStr(&time_str), iface, port);
  }

  /// Accept new connections (non-blocking) and reap any that disconnected.
  auto TCPServer::poll() noexcept -> void {
    // Drain the accept queue.
    while (true) {
      sockaddr_in addr{};
      socklen_t   addr_len = sizeof(addr);
      const int   client_fd = ::accept(listener_socket_.socket_fd_,
                                       reinterpret_cast<sockaddr *>(&addr),
                                       &addr_len);
      if (client_fd < 0) break;  // EAGAIN / EWOULDBLOCK -> no more connections

      setNonBlocking(client_fd);
      disableNagle(client_fd);

      auto *sock          = new TCPSocket(logger_);
      sock->socket_fd_    = client_fd;
      sock->socket_attrib_ = addr;
      sock->recv_callback_ = recv_callback_;
      receive_sockets_.push_back(sock);
      send_sockets_.push_back(sock);

      std::string time_str;
      logger_.log("%:% %() % accepted fd:%\n", __FILE__, __LINE__, __FUNCTION__,
                  getCurrentTimeStr(&time_str), client_fd);
    }
  }

  /// One pass: invoke sendAndRecv() on every connected socket. If any of them
  /// produced new RX bytes (TCPSocket already fired recv_callback_ inline),
  /// fire recv_finished_callback_ once at the end.
  auto TCPServer::sendAndRecv() noexcept -> void {
    bool any_rx = false;
    for (auto *sock : receive_sockets_) {
      if (sock->sendAndRecv()) any_rx = true;
    }
    if (any_rx && recv_finished_callback_) recv_finished_callback_();
  }
}
