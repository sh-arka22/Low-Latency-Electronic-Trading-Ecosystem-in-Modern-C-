#pragma once

// TEMPORARY Chapter 8/9 stub. The real TradeEngine is implemented in
// Chapter 10 and will replace this file. It exists here so
// market_order_book.cpp and order_manager.cpp can compile in isolation
// — once Ch10 lands, this stub is overwritten.

#include "common/types.h"
#include "exchange/order_server/client_request.h"
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

    auto clientId() const noexcept -> ClientId { return client_id_; }

    auto sendClientRequest(const Exchange::MEClientRequest * /*req*/) noexcept -> void {}

  private:
    ClientId client_id_ = ClientId_INVALID;
  };
}
