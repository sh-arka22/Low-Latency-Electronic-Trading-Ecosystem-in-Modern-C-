#pragma once

#include "common/macros.h"
#include "common/logging.h"
#include "strategy/order_manager.h"
#include "strategy/feature_engine.h"

using namespace Common;

namespace Trading {
  class TradeEngine;

  class LiquidityTaker {
  public:
    LiquidityTaker(Common::Logger *logger, TradeEngine *trade_engine,
                   FeatureEngine *feature_engine,
                   OrderManager *order_manager,
                   const TradeEngineCfgHashMap &ticker_cfg);

    auto onOrderBookUpdate(TickerId ticker_id, Price price,
                           Side side, MarketOrderBook *book) noexcept -> void;

    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update,
                       MarketOrderBook *book) noexcept -> void;

    auto onOrderUpdate(const Exchange::MEClientResponse *client_response)
        noexcept -> void;

    LiquidityTaker()                                   = delete;
    LiquidityTaker(const LiquidityTaker &)             = delete;
    LiquidityTaker(const LiquidityTaker &&)            = delete;
    LiquidityTaker &operator=(const LiquidityTaker &)  = delete;
    LiquidityTaker &operator=(const LiquidityTaker &&) = delete;

  private:
    const FeatureEngine *feature_engine_ = nullptr;
    OrderManager        *order_manager_  = nullptr;
    std::string          time_str_;
    Common::Logger      *logger_         = nullptr;
    const TradeEngineCfgHashMap ticker_cfg_;
  };
}
