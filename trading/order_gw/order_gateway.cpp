#include "order_gw/order_gateway.h"

#include <cerrno>

namespace Trading {
  OrderGateway::OrderGateway(ClientId client_id,
                             Exchange::ClientRequestLFQueue *client_requests,
                             Exchange::ClientResponseLFQueue *client_responses,
                             std::string ip, const std::string &iface, int port)
      : client_id_(client_id),
        ip_(ip),
        iface_(iface),
        port_(port),
        outgoing_requests_(client_requests),
        incoming_responses_(client_responses),
        logger_("trading_order_gateway_" + std::to_string(client_id) + ".log"),
        tcp_socket_(logger_) {
    tcp_socket_.recv_callback_ = [this](auto socket, auto rx_time) {
      recvCallback(socket, rx_time);
    };
  }

  // --------------------------------------------------------------------
  // Main loop. Two responsibilities per iteration:
  //   1. sendAndRecv() flushes buffered outbound bytes and triggers the
  //      recv callback for any inbound responses.
  //   2. Drain outgoing_requests_ from the trade engine and push each one
  //      onto the socket as [seq_num][MEClientRequest] — assembling the
  //      OMClientRequest wire format without a temporary struct.
  // --------------------------------------------------------------------
  auto OrderGateway::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
    while (run_) {
      tcp_socket_.sendAndRecv();

      for (auto client_request = outgoing_requests_->getNextToRead();
           client_request;
           client_request = outgoing_requests_->getNextToRead()) {

        tcp_socket_.send(&next_outgoing_seq_num_, sizeof(next_outgoing_seq_num_));
        tcp_socket_.send(client_request, sizeof(Exchange::MEClientRequest));
        outgoing_requests_->updateReadIndex();
        ++next_outgoing_seq_num_;
      }
    }
  }

  // --------------------------------------------------------------------
  // Parse OMClientResponses out of the TCP recv buffer. We validate both
  // client_id (must match ours) and seq_num (must be next_exp_seq_num_) on
  // every message. Mismatches are logged and dropped — TCP is reliable so
  // this would indicate an exchange-side bug rather than a transport issue,
  // and we don't want to crash on it.
  // --------------------------------------------------------------------
  auto OrderGateway::recvCallback(Common::TCPSocket *socket, Nanos /*rx_time*/) noexcept -> void {
    if (socket->next_rcv_valid_index_ >= sizeof(Exchange::OMClientResponse)) {
      size_t i = 0;
      for (; i + sizeof(Exchange::OMClientResponse) <= socket->next_rcv_valid_index_;
           i += sizeof(Exchange::OMClientResponse)) {
        auto response = reinterpret_cast<const Exchange::OMClientResponse *>(
            socket->inbound_data_.data() + i);

        if (response->me_client_response_.client_id_ != client_id_) {
          logger_.log("%:% %() % Dropping response for wrong client. got:% expected:%\n",
                      __FILE__, __LINE__, __FUNCTION__,
                      Common::getCurrentTimeStr(&time_str_),
                      response->me_client_response_.client_id_, client_id_);
          continue;
        }

        if (response->seq_num_ != next_exp_seq_num_) {
          logger_.log("%:% %() % Dropping out-of-sequence response. got:% expected:%\n",
                      __FILE__, __LINE__, __FUNCTION__,
                      Common::getCurrentTimeStr(&time_str_),
                      response->seq_num_, next_exp_seq_num_);
          continue;
        }

        ++next_exp_seq_num_;

        auto next_write = incoming_responses_->getNextToWriteTo();
        *next_write = std::move(response->me_client_response_);
        incoming_responses_->updateWriteIndex();
      }
      // Slide any trailing partial response left in the buffer.
      memcpy(socket->inbound_data_.data(),
             socket->inbound_data_.data() + i,
             socket->next_rcv_valid_index_ - i);
      socket->next_rcv_valid_index_ -= i;
    }
  }
}
