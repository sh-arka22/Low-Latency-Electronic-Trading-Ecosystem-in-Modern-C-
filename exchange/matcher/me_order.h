#pragma once

#include <array>
#include <sstream>
#include <string>

#include "common/types.h"

using namespace Common;

namespace Exchange {
  /// One live order resting in the book. Linked into a circular doubly linked
  /// list within its price level to preserve FIFO order priority.
  struct MEOrder {
    TickerId ticker_id_       = TickerId_INVALID;
    ClientId client_id_       = ClientId_INVALID;
    OrderId  client_order_id_ = OrderId_INVALID;  // id chosen by client
    OrderId  market_order_id_ = OrderId_INVALID;  // unique id assigned by ME
    Side     side_            = Side::INVALID;
    Price    price_           = Price_INVALID;
    Qty      qty_             = Qty_INVALID;      // remaining qty
    Priority priority_        = Priority_INVALID; // FIFO position at this price

    MEOrder *prev_order_ = nullptr;
    MEOrder *next_order_ = nullptr;

    MEOrder() = default;  // required by MemPool's pre-construction

    MEOrder(TickerId ticker_id, ClientId client_id, OrderId client_order_id,
            OrderId market_order_id, Side side, Price price, Qty qty,
            Priority priority, MEOrder *prev_order, MEOrder *next_order) noexcept
        : ticker_id_(ticker_id), client_id_(client_id),
          client_order_id_(client_order_id), market_order_id_(market_order_id),
          side_(side), price_(price), qty_(qty), priority_(priority),
          prev_order_(prev_order), next_order_(next_order) {}

    auto toString() const -> std::string;
  };

  /// Hash maps backed by std::array (no std::unordered_map on hot paths).
  /// ClientId -> OrderId -> MEOrder*.
  typedef std::array<MEOrder *, ME_MAX_ORDER_IDS>      OrderHashMap;
  typedef std::array<OrderHashMap, ME_MAX_NUM_CLIENTS> ClientOrderHashMap;

  /// One price level: head of a circular order list, plus prev/next links to
  /// neighbouring price levels (sorted best-first per side).
  struct MEOrdersAtPrice {
    Side  side_  = Side::INVALID;
    Price price_ = Price_INVALID;

    MEOrder         *first_me_order_ = nullptr;
    MEOrdersAtPrice *prev_entry_     = nullptr;
    MEOrdersAtPrice *next_entry_     = nullptr;

    MEOrdersAtPrice() = default;

    MEOrdersAtPrice(Side side, Price price, MEOrder *first_me_order,
                    MEOrdersAtPrice *prev_entry, MEOrdersAtPrice *next_entry)
        : side_(side), price_(price), first_me_order_(first_me_order),
          prev_entry_(prev_entry), next_entry_(next_entry) {}

    auto toString() const {
      std::stringstream ss;
      ss << "MEOrdersAtPrice["
         << "side:"            << sideToString(side_)   << " "
         << "price:"           << priceToString(price_) << " "
         << "first_me_order:"  << (first_me_order_ ? first_me_order_->toString() : "null") << " "
         << "prev:"            << priceToString(prev_entry_ ? prev_entry_->price_ : Price_INVALID) << " "
         << "next:"            << priceToString(next_entry_ ? next_entry_->price_ : Price_INVALID)
         << "]";
      return ss.str();
    }
  };

  /// Price -> MEOrdersAtPrice*, indexed by (price % ME_MAX_PRICE_LEVELS).
  typedef std::array<MEOrdersAtPrice *, ME_MAX_PRICE_LEVELS> OrdersAtPriceHashMap;
}
