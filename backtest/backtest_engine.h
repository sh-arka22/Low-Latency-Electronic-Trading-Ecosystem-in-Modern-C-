#pragma once

#include <fstream>
#include <memory>
#include <string>

#include "common/logging.h"
#include "common/lf_queue.h"
#include "common/types.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "strategy/trade_engine.h"

#include "backtest/binance_tape_reader.h"
#include "backtest/lobster_tape_reader.h"

namespace Backtest {

  // Drives a real TradeEngine + MarketMaker against a replayed tape, with a
  // simple queue-aware fill simulator. No threads, no sockets — the harness
  // is synchronous so a 6-hour replay finishes in seconds.
  //
  // The strategy code (FeatureEngine, MarketMaker, OrderManager, etc.) is the
  // *exact same* code used live, so any improvement measured here travels
  // straight to production.
  class BacktestEngine {
  public:
    struct Config {
      std::string  strategy_label;   // baseline / as / as_ofi / full
      std::string  pnl_csv_path;
      Common::TickerId ticker_id = 0;
      Common::TradeEngineCfg cfg{};

      // Tape config — passthrough to TapeReader.
      TapeReader::Config tape;

      // Queue model.
      double queue_decay_per_trade = 1.0;  // how many lots of `ahead_qty` a
                                            // trade at our level burns down.
    };

    explicit BacktestEngine(Config cfg);
    ~BacktestEngine();

    auto run() -> void;

    BacktestEngine(const BacktestEngine &)            = delete;
    BacktestEngine &operator=(const BacktestEngine &) = delete;

  private:
    Config cfg_;

    Common::Logger logger_;

    // LFQueues — the TradeEngine wants real ones; we drain them in-process.
    Exchange::ClientRequestLFQueue   client_requests_;
    Exchange::ClientResponseLFQueue  client_responses_;
    Exchange::MEMarketUpdateLFQueue  market_updates_;

    std::unique_ptr<Trading::TradeEngine> engine_;

    std::unique_ptr<TapeReader>    tape_;
    std::unique_ptr<LobsterReader> lobster_;   // native L3 (LOBSTER) replay path

    // Synthetic resting order IDs for replayed BBO updates (one per side).
    // We only ever have two live synth orders (bid + ask), so we recycle
    // through a small window of OIDs that won't collide with the algo's
    // OrderManager OIDs (which start at 1 and increment forward).
    Common::OrderId next_synth_oid_ = 500'000;
    auto nextSynthOid() noexcept -> Common::OrderId {
      const auto oid = next_synth_oid_++;
      // Stay inside [500K, 900K) — well clear of algo OIDs and ME_MAX_ORDER_IDS.
      if (next_synth_oid_ >= 900'000) next_synth_oid_ = 500'000;
      return oid;
    }
    Common::OrderId resting_bid_oid_ = Common::OrderId_INVALID;
    Common::OrderId resting_ask_oid_ = Common::OrderId_INVALID;
    Common::Price   last_bid_price_  = Common::Price_INVALID;
    Common::Price   last_ask_price_  = Common::Price_INVALID;
    Common::Qty     last_bid_qty_    = 0;
    Common::Qty     last_ask_qty_    = 0;

    // Live algo orders that we need to simulate fills for.
    struct LiveAlgoOrder {
      Common::OrderId order_id = Common::OrderId_INVALID;
      Common::Price   price    = Common::Price_INVALID;
      Common::Side    side     = Common::Side::INVALID;
      Common::Qty     leaves   = 0;
      Common::Qty     ahead    = 0;          // queue ahead of us at insert
      Common::Nanos   created_ns = 0;
    };
    // sideToIndex(Side::MAX)+1 — matches OMOrderTickerSideHashMap shape so BUY,
    // SELL, INVALID, MAX all map to valid slots without bounds-check throws.
    std::array<std::array<LiveAlgoOrder, Common::sideToIndex(Common::Side::MAX) + 1>,
               Common::ME_MAX_TICKERS> live_;

    // CSV writer.
    std::ofstream pnl_out_;
    uint64_t      pnl_row_count_ = 0;
    uint64_t      num_fills_     = 0;
    uint64_t      num_requotes_  = 0;

    // ----- helpers -----
    auto applyBookEvent(const BookTopEvent &ev) -> void;
    auto applyTradeEvent(const TradeEvent &ev) -> void;

    // Native L3 (LOBSTER) replay path.
    auto runLobster() -> void;
    auto syncBBOFromBook() -> void;

    auto pumpClientRequests(Common::Nanos now_ns) -> void;
    auto pumpClientResponses() -> void;

    auto matchAgainstBBO(Common::Nanos now_ns) -> void;
    auto matchAgainstTrade(const TradeEvent &ev) -> void;

    auto recordPnL(Common::Nanos now_ns) -> void;

    auto publishMarketUpdate(const Exchange::MEMarketUpdate &mu) -> void;
    auto publishResponse(const Exchange::MEClientResponse &cr) -> void;
  };
}
