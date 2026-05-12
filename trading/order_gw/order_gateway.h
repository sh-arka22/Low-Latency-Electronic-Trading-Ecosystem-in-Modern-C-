#pragma once

#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "common/logging.h"
#include "common/macros.h"
#include "common/tcp_socket.h"
#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "common/types.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"

using namespace Common;

namespace Trading {
  /// Mirror image of OrderServer (Ch7): connects to the exchange instead of
  /// listening. Owns exactly one TCPSocket — one persistent connection per
  /// client. Drains outgoing_requests_ from the trade engine and writes the
  /// wire format (seq_num + MEClientRequest) to TCP; parses OMClientResponse
  /// off the wire and forwards MEClientResponse via incoming_responses_.
  class OrderGateway {
  public:
    OrderGateway(ClientId client_id,
                 Exchange::ClientRequestLFQueue *client_requests,
                 Exchange::ClientResponseLFQueue *client_responses,
                 std::string ip, const std::string &iface, int port);

    ~OrderGateway() {
      stop();
      using namespace std::literals::chrono_literals;
      std::this_thread::sleep_for(5s);
    }

    auto start() {
      run_ = true;
      ASSERT(tcp_socket_.connect(ip_, iface_, port_, false) >= 0,
             "Unable to connect to ip:" + ip_ + " port:" + std::to_string(port_) +
                 " on iface:" + iface_ +
                 " error:" + std::string(std::strerror(errno)));
      ASSERT(Common::createAndStartThread(-1, "Trading/OrderGateway",
                                          [this]() { run(); }) != nullptr,
             "Failed to start OrderGateway thread.");
    }

    auto stop() -> void { run_ = false; }

    OrderGateway() = delete;
    OrderGateway(const OrderGateway &) = delete;
    OrderGateway(const OrderGateway &&) = delete;
    OrderGateway &operator=(const OrderGateway &) = delete;
    OrderGateway &operator=(const OrderGateway &&) = delete;

  private:
    const ClientId    client_id_;
    std::string       ip_;
    const std::string iface_;
    const int         port_ = 0;

    Exchange::ClientRequestLFQueue  *outgoing_requests_  = nullptr;
    Exchange::ClientResponseLFQueue *incoming_responses_ = nullptr;

    volatile bool run_ = false;
    std::string   time_str_;
    Logger        logger_;

    /// Sequence numbers stamped onto outgoing OMClientRequests / validated
    /// on incoming OMClientResponses. Start at 1 to match OrderServer.
    size_t next_outgoing_seq_num_ = 1;
    size_t next_exp_seq_num_      = 1;

    Common::TCPSocket tcp_socket_;

  private:
    auto run() noexcept -> void;
    auto recvCallback(Common::TCPSocket *socket, Nanos rx_time) noexcept -> void;
  };
}
