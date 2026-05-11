#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/logging.h"
#include "common/macros.h"

namespace Common {
  /// Inputs to createSocket(). Carries everything the kernel needs.
  struct SocketCfg {
    std::string ip_;
    std::string iface_;
    int  port_                = -1;
    bool is_udp_              = false;
    bool is_listening_        = false;
    bool needs_so_timestamp_  = false;

    auto toString() const {
      std::stringstream ss;
      ss << "SocketCfg[ip:" << ip_
         << " iface:"             << iface_
         << " port:"              << port_
         << " is_udp:"            << is_udp_
         << " is_listening:"      << is_listening_
         << " needs_SO_timestamp:" << needs_so_timestamp_
         << "]";
      return ss.str();
    }
  };

  constexpr int MaxTCPServerBacklog = 1024;

  /// "eth0" / "lo" -> "10.0.0.1" / "127.0.0.1". Empty if interface not found.
  inline auto getIfaceIP(const std::string &iface) -> std::string {
    char      buf[NI_MAXHOST] = {'\0'};
    ifaddrs  *ifaddr          = nullptr;

    if (getifaddrs(&ifaddr) != -1) {
      for (ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
            iface == ifa->ifa_name) {
          getnameinfo(ifa->ifa_addr, sizeof(sockaddr_in), buf, sizeof(buf),
                      nullptr, 0, NI_NUMERICHOST);
          break;
        }
      }
      freeifaddrs(ifaddr);
    }
    return buf;
  }

  /// Recv/send return EAGAIN instead of blocking; caller drives the poll loop.
  inline auto setNonBlocking(int fd) -> bool {
    const auto flags = fcntl(fd, F_GETFL, 0);
    if (flags & O_NONBLOCK) return true;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
  }

  /// Disable Nagle: small writes are flushed immediately (latency > throughput).
  inline auto disableNagle(int fd) -> bool {
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<void *>(&one), sizeof(one)) != -1;
  }

  /// Kernel software RX timestamps. Linux only; macOS has no equivalent.
  inline auto setSOTimestamp(int fd) -> bool {
#if defined(__linux__) && defined(SO_TIMESTAMP)
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP,
                      reinterpret_cast<void *>(&one), sizeof(one)) != -1;
#else
    (void)fd;
    return true;  // best-effort no-op
#endif
  }

  /// Join a multicast group on the default interface.
  inline auto join(int fd, const std::string &ip) -> bool {
    const ip_mreq mreq{{inet_addr(ip.c_str())}, {htonl(INADDR_ANY)}};
    return setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != -1;
  }

  /// Create a TCP or UDP socket. Returns fd, or aborts via ASSERT on failure.
  [[nodiscard]] inline auto createSocket(Logger &logger, const SocketCfg &cfg) -> int {
    std::string time_str;
    const auto  ip = cfg.ip_.empty() ? getIfaceIP(cfg.iface_) : cfg.ip_;

    logger.log("%:% %() % cfg:%\n", __FILE__, __LINE__, __FUNCTION__,
               Common::getCurrentTimeStr(&time_str), cfg.toString());

    const int input_flags =
        (cfg.is_listening_ ? AI_PASSIVE : 0) | (AI_NUMERICHOST | AI_NUMERICSERV);
    const addrinfo hints{input_flags, AF_INET,
                         cfg.is_udp_ ? SOCK_DGRAM : SOCK_STREAM,
                         cfg.is_udp_ ? IPPROTO_UDP : IPPROTO_TCP,
                         0, 0, nullptr, nullptr};

    addrinfo *result = nullptr;
    const auto rc = getaddrinfo(ip.c_str(),
                                std::to_string(cfg.port_).c_str(),
                                &hints, &result);
    ASSERT(rc == 0,
           "getaddrinfo() failed. error:" + std::string(gai_strerror(rc)) +
               " errno:" + std::strerror(errno));

    int socket_fd = -1;
    int one       = 1;
    for (addrinfo *rp = result; rp; rp = rp->ai_next) {
      ASSERT((socket_fd = ::socket(rp->ai_family, rp->ai_socktype,
                                   rp->ai_protocol)) != -1,
             "socket() failed. errno:" + std::string(std::strerror(errno)));

      ASSERT(setNonBlocking(socket_fd),
             "setNonBlocking() failed. errno:" + std::string(std::strerror(errno)));

      if (!cfg.is_udp_) {
        ASSERT(disableNagle(socket_fd),
               "disableNagle() failed. errno:" + std::string(std::strerror(errno)));
      }

      if (!cfg.is_listening_) {
        // For TCP this connects; for UDP it sets the default destination.
        // The book tolerates EINPROGRESS by checking != 1 (allow async connect).
        if (::connect(socket_fd, rp->ai_addr, rp->ai_addrlen) == -1 &&
            errno != EINPROGRESS) {
          // For UDP "connect", errors are rare; for TCP EINPROGRESS is normal.
        }
      }

      if (cfg.is_listening_) {
        ASSERT(setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                          reinterpret_cast<const char *>(&one), sizeof(one)) == 0,
               "SO_REUSEADDR failed. errno:" + std::string(std::strerror(errno)));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(static_cast<uint16_t>(cfg.port_));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        ASSERT(::bind(socket_fd,
                      cfg.is_udp_
                          ? reinterpret_cast<const struct sockaddr *>(&addr)
                          : rp->ai_addr,
                      sizeof(addr)) == 0,
               "bind() failed. errno:" + std::string(std::strerror(errno)));
      }

      if (!cfg.is_udp_ && cfg.is_listening_) {
        ASSERT(::listen(socket_fd, MaxTCPServerBacklog) == 0,
               "listen() failed. errno:" + std::string(std::strerror(errno)));
      }

      if (cfg.needs_so_timestamp_) {
        ASSERT(setSOTimestamp(socket_fd),
               "setSOTimestamp() failed. errno:" + std::string(std::strerror(errno)));
      }
    }

    if (result) freeaddrinfo(result);
    return socket_fd;
  }
}
