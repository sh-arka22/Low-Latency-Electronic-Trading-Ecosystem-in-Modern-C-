#include "matcher/matching_engine.h"

#include <chrono>
#include <thread>

namespace Exchange {
  MatchingEngine::MatchingEngine(ClientRequestLFQueue *client_requests,
                                 ClientResponseLFQueue *client_responses,
                                 MEMarketUpdateLFQueue *market_updates)
      : incoming_requests_(client_requests), // store pointer to request queue for the OGS thread
        outgoing_ogw_responses_(client_responses), // store pointer to response queue for the OGS thread
        outgoing_md_updates_(market_updates), // store pointer to market update queue for the MDP thread
        logger_("exchange_matching_engine.log") // second Logger + second flush thread for the matching engine
      { 
      for (size_t i = 0; i < ticker_order_book_.size(); ++i) {
        ticker_order_book_[i] = new MEOrderBook(i, &logger_, this);
      }
  }

  MatchingEngine::~MatchingEngine() {
    run_ = false;

    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);  // let run() exit.

    incoming_requests_      = nullptr;
    outgoing_ogw_responses_ = nullptr;
    outgoing_md_updates_    = nullptr;

    for (auto &book : ticker_order_book_) {
      delete book;
      book = nullptr;
    }
  }

  auto MatchingEngine::start() -> void {
    run_ = true;
    ASSERT(Common::createAndStartThread(-1, "Exchange/MatchingEngine",
                                        [this]() { run(); }) != nullptr,
           "Failed to start MatchingEngine thread.");
  }

  auto MatchingEngine::stop() -> void {
    run_ = false;
  }
}
