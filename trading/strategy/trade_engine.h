#pragma once

// TEMPORARY Chapter 8 stub. The real TradeEngine is implemented in Chapter 9
// and will replace this file. It exists here only so market_order_book.cpp
// can compile in isolation — once Ch9 lands, this stub is overwritten.

#include "common/types.h"
#include "market_data/market_update.h"
#include "strategy/market_order.h"

using namespace Common;

namespace Trading {
  class MarketOrderBook;

  class TradeEngine {
  public:
    auto onTradeUpdate(const Exchange::MEMarketUpdate * /*update*/,
                       MarketOrderBook * /*book*/) noexcept -> void {}

    auto onOrderBookUpdate(TickerId /*ticker_id*/, Price /*price*/, Side /*side*/,
                           MarketOrderBook * /*book*/) noexcept -> void {}
  };
}
