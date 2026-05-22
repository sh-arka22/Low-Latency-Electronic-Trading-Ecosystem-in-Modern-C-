#pragma once

#include <string>

#include "common/lf_queue.h"
#include "common/logging.h"
#include "common/macros.h"
#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "market_data/market_update.h"
#include "matcher/me_order_book.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"

namespace Exchange {
  /// Owns a MEOrderBook per ticker and runs the single matching-engine thread.
  /// All cross-thread traffic is via three LFQueues:
  ///   - incoming_requests_       : OGS thread  -> here
  ///   - outgoing_ogw_responses_  : here        -> OGS thread
  ///   - outgoing_md_updates_     : here        -> MDP thread
  class MatchingEngine final {
  public:
    MatchingEngine(ClientRequestLFQueue *client_requests,
                   ClientResponseLFQueue *client_responses,
                   MEMarketUpdateLFQueue *market_updates);

    ~MatchingEngine();

    auto start() -> void;
    auto stop()  -> void;

    /// Dispatch one client request to the order book for its ticker.
    auto processClientRequest(const MEClientRequest *req) noexcept {
      auto order_book = ticker_order_book_[req->ticker_id_];
      switch (req->type_) {
        case ClientRequestType::NEW: {
          START_MEASURE(Exchange_MEOrderBook_add);
          order_book->add(req->client_id_, req->order_id_, req->ticker_id_,
                          req->side_, req->price_, req->qty_);
          END_MEASURE(Exchange_MEOrderBook_add, logger_);
        } break;
        case ClientRequestType::CANCEL: {
          START_MEASURE(Exchange_MEOrderBook_cancel);
          order_book->cancel(req->client_id_, req->order_id_, req->ticker_id_);
          END_MEASURE(Exchange_MEOrderBook_cancel, logger_);
        } break;
        default:
          FATAL("Received invalid client-request-type:" +
                clientRequestTypeToString(req->type_));
          break;
      }
    }

    /// Hand a private response off to the OGS thread.
    auto sendClientResponse(const MEClientResponse *client_response) noexcept {
      logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), client_response->toString());
      auto next_write = outgoing_ogw_responses_->getNextToWriteTo();
      *next_write     = std::move(*client_response);
      outgoing_ogw_responses_->updateWriteIndex();

      TTT_MEASURE(T4t_MatchingEngine_LFQueue_write, logger_);
    }

    /// Hand a public market update off to the MDP thread.
    auto sendMarketUpdate(const MEMarketUpdate *market_update) noexcept {
      logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), market_update->toString());
      auto next_write = outgoing_md_updates_->getNextToWriteTo();
      *next_write     = *market_update;
      outgoing_md_updates_->updateWriteIndex();

      TTT_MEASURE(T4_MatchingEngine_LFQueue_write, logger_);
    }

    /// Hot loop: drain the request queue.
    auto run() noexcept {
      logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_));
      while (run_) {
        const auto req = incoming_requests_->getNextToRead();
        if (LIKELY(req)) {
          TTT_MEASURE(T3_MatchingEngine_LFQueue_read, logger_);

          logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__,
                      __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                      req->toString());

          START_MEASURE(Exchange_MatchingEngine_processClientRequest);
          processClientRequest(req);
          END_MEASURE(Exchange_MatchingEngine_processClientRequest, logger_);

          incoming_requests_->updateReadIndex();
        }
      }
    }

    MatchingEngine() = delete;
    MatchingEngine(const MatchingEngine &) = delete;
    MatchingEngine(const MatchingEngine &&) = delete;
    MatchingEngine &operator=(const MatchingEngine &) = delete;
    MatchingEngine &operator=(const MatchingEngine &&) = delete;

  private:
    OrderBookHashMap ticker_order_book_;

    ClientRequestLFQueue  *incoming_requests_      = nullptr;
    ClientResponseLFQueue *outgoing_ogw_responses_ = nullptr;
    MEMarketUpdateLFQueue *outgoing_md_updates_    = nullptr;

    volatile bool run_ = false;  // visible across threads without a mutex.

    std::string time_str_;
    Logger      logger_;
  };
}
