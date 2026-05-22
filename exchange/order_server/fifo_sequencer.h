#pragma once

#include <algorithm>
#include <array>
#include <string>

#include "common/lf_queue.h"
#include "common/logging.h"
#include "common/macros.h"
#include "common/time_utils.h"
#include "common/types.h"
#include "order_server/client_request.h"

using namespace Common;

namespace Exchange {
  constexpr size_t ME_MAX_PENDING_REQUESTS = 1024;

  /// Solves fairness: TCP delivery order != arrival order. Stage every request
  /// from one sendAndRecv() round with its software RX timestamp, then sort
  /// by RX time and push to the matching engine.
  class FIFOSequencer final {
  public:
    FIFOSequencer(ClientRequestLFQueue *client_requests, Logger *logger)
        : incoming_requests_(client_requests), logger_(logger) {}

    /// Stash one decoded request + its hardware/software RX timestamp.
    /// Called once per request inside recvCallback().
    auto addClientRequest(Nanos rx_time, const MEClientRequest &request) {
      if (pending_size_ >= pending_client_requests_.size()) {
        FATAL("Too many pending requests");
      }
      pending_client_requests_.at(pending_size_++) =
          RecvTimeClientRequest{rx_time, request};
    }

    /// Sort the batch by RX timestamp and forward to matching engine.
    /// Called once per round inside recvFinishedCallback().
    auto sequenceAndPublish() {
      if (UNLIKELY(!pending_size_)) return;

      std::string time_str;
      logger_->log("%:% %() % Processing % pending requests.\n",
                   __FILE__, __LINE__, __FUNCTION__,
                   getCurrentTimeStr(&time_str), pending_size_);

      std::sort(pending_client_requests_.begin(),
                pending_client_requests_.begin() + pending_size_);

      for (size_t i = 0; i < pending_size_; ++i) {
        const auto &req   = pending_client_requests_.at(i);
        auto       *slot = incoming_requests_->getNextToWriteTo();
        *slot = req.request_;
        incoming_requests_->updateWriteIndex();

        TTT_MEASURE(T2_OrderServer_LFQueue_write, (*logger_));
      }
      pending_size_ = 0;
    }

    FIFOSequencer() = delete;
    FIFOSequencer(const FIFOSequencer &) = delete;
    FIFOSequencer(const FIFOSequencer &&) = delete;
    FIFOSequencer &operator=(const FIFOSequencer &) = delete;
    FIFOSequencer &operator=(const FIFOSequencer &&) = delete;

  private:
    ClientRequestLFQueue *incoming_requests_ = nullptr;  // -> matching engine
    std::string           time_str_;
    Logger               *logger_ = nullptr;

    struct RecvTimeClientRequest {
      Nanos           recv_time_ = 0;
      MEClientRequest request_{};

      bool operator<(const RecvTimeClientRequest &rhs) const {
        return recv_time_ < rhs.recv_time_;
      }
    };

    std::array<RecvTimeClientRequest, ME_MAX_PENDING_REQUESTS>
        pending_client_requests_;
    size_t pending_size_ = 0;
  };
}
