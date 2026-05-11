#pragma once

#include <array>
#include <cstring>
#include <string>

#include "common/lf_queue.h"
#include "common/logging.h"
#include "common/macros.h"
#include "common/tcp_server.h"
#include "common/tcp_socket.h"
#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "common/types.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "order_server/fifo_sequencer.h"

namespace Exchange {
  /// TCP order gateway. Owns the listener socket, decodes OMClientRequest off
  /// the wire, hands each request to FIFOSequencer with its RX timestamp,
  /// drains MatchingEngine responses and writes OMClientResponse back.
  class OrderServer {
  public:
    OrderServer(ClientRequestLFQueue *client_requests,
                ClientResponseLFQueue *client_responses,
                const std::string &iface, int port);

    ~OrderServer();

    auto start() -> void;
    auto stop()  -> void;

    /// Single-thread loop: poll for connections, drain sockets, ship responses.
    auto run() noexcept {
      logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_));
      while (run_) {
        tcp_server_.poll();          // accept new conns / reap dead ones
        tcp_server_.sendAndRecv();   // RX -> recvCallback per socket, then recvFinishedCallback

        // Drain ME -> client responses.
        for (auto client_response = outgoing_responses_->getNextToRead();
             outgoing_responses_->size() && client_response;
             client_response = outgoing_responses_->getNextToRead()) {

          auto &next_outgoing_seq_num =
              cid_next_outgoing_seq_num_[client_response->client_id_];

          logger_.log("%:% %() % Processing cid:% seq:% %\n",
                      __FILE__, __LINE__, __FUNCTION__,
                      Common::getCurrentTimeStr(&time_str_),
                      client_response->client_id_, next_outgoing_seq_num,
                      client_response->toString());

          ASSERT(cid_tcp_socket_[client_response->client_id_] != nullptr,
                 "Dropping response to unknown client_id:" +
                 std::to_string(client_response->client_id_));

          // Assemble OMClientResponse on the wire as two staged sends:
          //   [seq_num_][MEClientResponse]
          cid_tcp_socket_[client_response->client_id_]->send(
              &next_outgoing_seq_num, sizeof(next_outgoing_seq_num));
          cid_tcp_socket_[client_response->client_id_]->send(
              client_response, sizeof(MEClientResponse));

          outgoing_responses_->updateReadIndex();
          ++next_outgoing_seq_num;
        }
      }
    }

    /// Invoked by TCPServer for every socket that received bytes this round.
    /// Decodes whole OMClientRequest messages out of the inbound buffer,
    /// validates seq + socket identity, then stages each into FIFOSequencer.
    auto recvCallback(Common::TCPSocket *socket, Common::Nanos rx_time) noexcept {
      logger_.log("%:% %() % Received fd:% len:% rx:%\n", __FILE__, __LINE__,
                  __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                  socket->socket_fd_, socket->next_rcv_valid_index_, rx_time);

      if (socket->next_rcv_valid_index_ >= sizeof(OMClientRequest)) {
        size_t i = 0;
        for (; i + sizeof(OMClientRequest) <= socket->next_rcv_valid_index_;
             i += sizeof(OMClientRequest)) {

          const auto *request = reinterpret_cast<const OMClientRequest *>(
              socket->inbound_data_.data() + i);

          // First message from this client: bind ClientId -> socket.
          if (UNLIKELY(cid_tcp_socket_[request->me_client_request_.client_id_] ==
                       nullptr)) {
            cid_tcp_socket_[request->me_client_request_.client_id_] = socket;
          }

          // Reject same ClientId on a different socket (impersonation guard).
          if (cid_tcp_socket_[request->me_client_request_.client_id_] != socket) {
            logger_.log("%:% %() % ClientId:% on wrong socket fd:%, ignoring.\n",
                        __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_),
                        request->me_client_request_.client_id_,
                        socket->socket_fd_);
            continue;
          }

          // Sequence gap / replay protection.
          auto &next_exp_seq_num =
              cid_next_exp_seq_num_[request->me_client_request_.client_id_];
          if (request->seq_num_ != next_exp_seq_num) {
            logger_.log("%:% %() % Bad seq cid:% expected:% got:% ignored.\n",
                        __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_),
                        request->me_client_request_.client_id_,
                        next_exp_seq_num, request->seq_num_);
            continue;
          }
          ++next_exp_seq_num;

          fifo_sequencer_.addClientRequest(rx_time,
                                           request->me_client_request_);
        }

        // Compact any partial trailing message back to the front of the buffer.
        std::memmove(socket->inbound_data_.data(),
                     socket->inbound_data_.data() + i,
                     socket->next_rcv_valid_index_ - i);
        socket->next_rcv_valid_index_ -= i;
      }
    }

    /// Invoked once per sendAndRecv() round after all sockets have been drained.
    auto recvFinishedCallback() noexcept {
      fifo_sequencer_.sequenceAndPublish();
    }

    OrderServer() = delete;
    OrderServer(const OrderServer &) = delete;
    OrderServer(const OrderServer &&) = delete;
    OrderServer &operator=(const OrderServer &) = delete;
    OrderServer &operator=(const OrderServer &&) = delete;

  private:
    const std::string iface_;
    const int         port_ = 0;

    ClientResponseLFQueue *outgoing_responses_ = nullptr;  // from matching engine

    volatile bool run_ = false;
    std::string   time_str_;
    Logger        logger_;

    // Flat hash maps keyed by ClientId (uint32_t, max 256).
    std::array<size_t, ME_MAX_NUM_CLIENTS>              cid_next_outgoing_seq_num_;
    std::array<size_t, ME_MAX_NUM_CLIENTS>              cid_next_exp_seq_num_;
    std::array<Common::TCPSocket *, ME_MAX_NUM_CLIENTS> cid_tcp_socket_;

    Common::TCPServer tcp_server_;
    FIFOSequencer     fifo_sequencer_;
  };
}
