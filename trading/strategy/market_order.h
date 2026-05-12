#pragma once

#include <array>
#include <sstream>
#include <string>

#include "common/types.h"

using namespace Common;

namespace Trading {
  /// Client-side mirror of MEOrder. Same circular doubly-linked list pattern
  /// inside its price level, but stripped of fields the client doesn't need
  /// (ticker_id, client_id, market vs client order ids — there's only one id
  /// here because the client sees the matching engine's market_order_id).
  struct MarketOrder {
    OrderId  order_id_ = OrderId_INVALID;
    Side     side_     = Side::INVALID;
    Price    price_    = Price_INVALID;
    Qty      qty_      = Qty_INVALID;
    Priority priority_ = Priority_INVALID;

    MarketOrder *prev_order_ = nullptr;
    MarketOrder *next_order_ = nullptr;

    MarketOrder() = default;  // required by MemPool

    MarketOrder(OrderId order_id, Side side, Price price, Qty qty,
                Priority priority, MarketOrder *prev_order,
                MarketOrder *next_order) noexcept
        : order_id_(order_id), side_(side), price_(price), qty_(qty),
          priority_(priority), prev_order_(prev_order), next_order_(next_order) {}

    auto toString() const -> std::string;
  };

  /// OrderId -> MarketOrder*. The client doesn't multiplex by ClientId, so
  /// the second dimension of the exchange's ClientOrderHashMap is dropped.
  typedef std::array<MarketOrder *, ME_MAX_ORDER_IDS> OrderHashMap;

  /// One price level on the client view of the book.
  struct MarketOrdersAtPrice {
    Side  side_  = Side::INVALID;
    Price price_ = Price_INVALID;

    MarketOrder         *first_mkt_order_ = nullptr;
    MarketOrdersAtPrice *prev_entry_      = nullptr;
    MarketOrdersAtPrice *next_entry_      = nullptr;

    MarketOrdersAtPrice() = default;

    MarketOrdersAtPrice(Side side, Price price, MarketOrder *first_mkt_order,
                        MarketOrdersAtPrice *prev_entry,
                        MarketOrdersAtPrice *next_entry)
        : side_(side), price_(price), first_mkt_order_(first_mkt_order),
          prev_entry_(prev_entry), next_entry_(next_entry) {}

    auto toString() const {
      std::stringstream ss;
      ss << "MarketOrdersAtPrice["
         << "side:"            << sideToString(side_)   << " "
         << "price:"           << priceToString(price_) << " "
         << "first_mkt_order:" << (first_mkt_order_ ? first_mkt_order_->toString() : "null") << " "
         << "prev:"            << priceToString(prev_entry_ ? prev_entry_->price_ : Price_INVALID) << " "
         << "next:"            << priceToString(next_entry_ ? next_entry_->price_ : Price_INVALID)
         << "]";
      return ss.str();
    }
  };

  typedef std::array<MarketOrdersAtPrice *, ME_MAX_PRICE_LEVELS> OrdersAtPriceHashMap;

  /// Best bid / best ask summary maintained inside MarketOrderBook so downstream
  /// strategy components don't re-walk the price-level list on every query.
  struct BBO {
    Price bid_price_ = Price_INVALID, ask_price_ = Price_INVALID;
    Qty   bid_qty_   = Qty_INVALID,   ask_qty_   = Qty_INVALID;

    auto toString() const {
      std::stringstream ss;
      ss << "BBO{" << qtyToString(bid_qty_) << "@" << priceToString(bid_price_)
         << "X"    << priceToString(ask_price_) << "@" << qtyToString(ask_qty_) << "}";
      return ss.str();
    }
  };
}
