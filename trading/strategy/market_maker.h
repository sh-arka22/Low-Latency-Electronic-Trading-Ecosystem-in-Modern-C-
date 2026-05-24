#pragma once

#include "common/macros.h"
#include "common/logging.h"
#include "common/time_utils.h"
#include "strategy/order_manager.h"
#include "strategy/feature_engine.h"
#include "strategy/position_keeper.h"

using namespace Common;

namespace Trading {
  class TradeEngine;   // forward declaration — full definition in trade_engine.h

  class MarketMaker {
  public:
    MarketMaker(Common::Logger *logger, TradeEngine *trade_engine,
                const FeatureEngine *feature_engine,
                const PositionKeeper *position_keeper,
                OrderManager *order_manager,
                const TradeEngineCfgHashMap &ticker_cfg);

    auto onOrderBookUpdate(TickerId ticker_id, Price price,
                           Side side, const MarketOrderBook *book) noexcept -> void;

    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update,
                       MarketOrderBook *book) noexcept -> void;

    auto onOrderUpdate(const Exchange::MEClientResponse *client_response)
        noexcept -> void;

    MarketMaker()                                = delete;
    MarketMaker(const MarketMaker &)             = delete;
    MarketMaker(const MarketMaker &&)            = delete;
    MarketMaker &operator=(const MarketMaker &)  = delete;
    MarketMaker &operator=(const MarketMaker &&) = delete;

  private:
    const FeatureEngine  *feature_engine_   = nullptr;
    const PositionKeeper *position_keeper_  = nullptr;
    OrderManager         *order_manager_    = nullptr;
    std::string           time_str_;
    Common::Logger       *logger_           = nullptr;
    const TradeEngineCfgHashMap ticker_cfg_;
    Nanos                 session_start_ns_ = 0;
  };
}
