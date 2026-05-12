#pragma once

#include <array>
#include <string>

#include "common/logging.h"
#include "common/macros.h"
#include "common/mem_pool.h"
#include "common/types.h"
#include "market_data/market_update.h"
#include "strategy/market_order.h"

using namespace Common;

namespace Trading {
  /// Forward decl — TradeEngine is built in Ch9. market_order_book.cpp will
  /// include trade_engine.h directly (a stub is provided in Ch8).
  class TradeEngine;

  /// Client-side Limit Order Book for a single ticker. Algorithms (addOrder,
  /// removeOrder, addOrdersAtPrice, removeOrdersAtPrice) mirror MEOrderBook
  /// exactly. The differences vs the matching engine:
  ///   - No matching logic — onMarketUpdate only mutates the book.
  ///   - Maintains a BBO summary (updated after every book change).
  ///   - Notifies TradeEngine on every TRADE / book update.
  ///   - Driven by the TradeEngine thread, no thread of its own.
  class MarketOrderBook final {
  public:
    explicit MarketOrderBook(TickerId ticker_id, Logger *logger);

    ~MarketOrderBook();

    auto onMarketUpdate(const Exchange::MEMarketUpdate *market_update) noexcept -> void;

    auto setTradeEngine(TradeEngine *trade_engine) { trade_engine_ = trade_engine; }

    /// Re-derive BBO for the side(s) whose top-of-book changed. Walks the
    /// circular per-price-level list to sum total quantity at the best price.
    auto updateBBO(bool update_bid, bool update_ask) noexcept {
      if (update_bid) {
        if (bids_by_price_) {
          bbo_.bid_price_ = bids_by_price_->price_;
          bbo_.bid_qty_   = bids_by_price_->first_mkt_order_->qty_;
          for (auto order = bids_by_price_->first_mkt_order_->next_order_;
               order != bids_by_price_->first_mkt_order_;
               order = order->next_order_) {
            bbo_.bid_qty_ += order->qty_;
          }
        } else {
          bbo_.bid_price_ = Price_INVALID;
          bbo_.bid_qty_   = Qty_INVALID;
        }
      }

      if (update_ask) {
        if (asks_by_price_) {
          bbo_.ask_price_ = asks_by_price_->price_;
          bbo_.ask_qty_   = asks_by_price_->first_mkt_order_->qty_;
          for (auto order = asks_by_price_->first_mkt_order_->next_order_;
               order != asks_by_price_->first_mkt_order_;
               order = order->next_order_) {
            bbo_.ask_qty_ += order->qty_;
          }
        } else {
          bbo_.ask_price_ = Price_INVALID;
          bbo_.ask_qty_   = Qty_INVALID;
        }
      }
    }

    auto getBBO() const noexcept -> const BBO * { return &bbo_; }

    auto toString(bool detailed, bool validity_check) const -> std::string;

    MarketOrderBook() = delete;
    MarketOrderBook(const MarketOrderBook &) = delete;
    MarketOrderBook(const MarketOrderBook &&) = delete;
    MarketOrderBook &operator=(const MarketOrderBook &) = delete;
    MarketOrderBook &operator=(const MarketOrderBook &&) = delete;

  private:
    const TickerId ticker_id_;
    TradeEngine   *trade_engine_ = nullptr;

    OrderHashMap                   oid_to_order_;          // O(1) by order_id
    MemPool<MarketOrdersAtPrice>   orders_at_price_pool_;
    MarketOrdersAtPrice           *bids_by_price_ = nullptr;  // descending
    MarketOrdersAtPrice           *asks_by_price_ = nullptr;  // ascending
    OrdersAtPriceHashMap           price_orders_at_price_;
    MemPool<MarketOrder>           order_pool_;

    BBO         bbo_;
    std::string time_str_;
    Logger     *logger_ = nullptr;

    // ---- private helpers (inline, mirror MEOrderBook) ----------------

    auto priceToIndex(Price price) const noexcept {
      return price % ME_MAX_PRICE_LEVELS;
    }

    auto getOrdersAtPrice(Price price) const noexcept -> MarketOrdersAtPrice * {
      return price_orders_at_price_.at(priceToIndex(price));
    }

    /// Insert a new price level into the appropriate side's sorted list.
    /// Bids descending, asks ascending; list is circular doubly-linked with
    /// head = best price.
    auto addOrdersAtPrice(MarketOrdersAtPrice *new_orders_at_price) noexcept {
      price_orders_at_price_.at(priceToIndex(new_orders_at_price->price_)) =
          new_orders_at_price;

      const auto best = (new_orders_at_price->side_ == Side::BUY ? bids_by_price_
                                                                 : asks_by_price_);

      if (UNLIKELY(!best)) {
        (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_)
            = new_orders_at_price;
        new_orders_at_price->prev_entry_ = new_orders_at_price->next_entry_ =
            new_orders_at_price;
        return;
      }

      auto target    = best;
      bool add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                        (new_orders_at_price->side_ == Side::BUY  && new_orders_at_price->price_ < target->price_));
      if (add_after) {
        target    = target->next_entry_;
        add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                     (new_orders_at_price->side_ == Side::BUY  && new_orders_at_price->price_ < target->price_));
      }
      while (add_after && target != best) {
        add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                     (new_orders_at_price->side_ == Side::BUY  && new_orders_at_price->price_ < target->price_));
        if (add_after) target = target->next_entry_;
      }

      if (add_after) {
        if (target == best) target = best->prev_entry_;
        new_orders_at_price->prev_entry_  = target;
        target->next_entry_->prev_entry_  = new_orders_at_price;
        new_orders_at_price->next_entry_  = target->next_entry_;
        target->next_entry_               = new_orders_at_price;
      } else {
        new_orders_at_price->prev_entry_ = target->prev_entry_;
        new_orders_at_price->next_entry_ = target;
        target->prev_entry_->next_entry_ = new_orders_at_price;
        target->prev_entry_              = new_orders_at_price;

        if ((new_orders_at_price->side_ == Side::BUY  && new_orders_at_price->price_ > best->price_) ||
            (new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ < best->price_)) {
          target->next_entry_ = (target->next_entry_ == best ? new_orders_at_price : target->next_entry_);
          (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_) =
              new_orders_at_price;
        }
      }
    }

    /// Drop an empty price level from its side's sorted list.
    auto removeOrdersAtPrice(Side side, Price price) noexcept {
      const auto best            = (side == Side::BUY ? bids_by_price_ : asks_by_price_);
      auto       orders_at_price = getOrdersAtPrice(price);

      if (UNLIKELY(orders_at_price->next_entry_ == orders_at_price)) {
        (side == Side::BUY ? bids_by_price_ : asks_by_price_) = nullptr;
      } else {
        orders_at_price->prev_entry_->next_entry_ = orders_at_price->next_entry_;
        orders_at_price->next_entry_->prev_entry_ = orders_at_price->prev_entry_;

        if (orders_at_price == best) {
          (side == Side::BUY ? bids_by_price_ : asks_by_price_) =
              orders_at_price->next_entry_;
        }
        orders_at_price->prev_entry_ = orders_at_price->next_entry_ = nullptr;
      }

      price_orders_at_price_.at(priceToIndex(price)) = nullptr;
      orders_at_price_pool_.deallocate(orders_at_price);
    }

    /// Unlink a resting order; if it was the last at its price, drop the level.
    auto removeOrder(MarketOrder *order) noexcept -> void {
      auto orders_at_price = getOrdersAtPrice(order->price_);

      if (order->prev_order_ == order) {
        removeOrdersAtPrice(order->side_, order->price_);
      } else {
        const auto before = order->prev_order_;
        const auto after  = order->next_order_;
        before->next_order_ = after;
        after->prev_order_  = before;

        if (orders_at_price->first_mkt_order_ == order) {
          orders_at_price->first_mkt_order_ = after;
        }
        order->prev_order_ = order->next_order_ = nullptr;
      }

      oid_to_order_.at(order->order_id_) = nullptr;
      order_pool_.deallocate(order);
    }

    /// Append a fresh order to its price level's circular list (FIFO tail).
    auto addOrder(MarketOrder *order) noexcept -> void {
      const auto orders_at_price = getOrdersAtPrice(order->price_);

      if (!orders_at_price) {
        order->next_order_ = order->prev_order_ = order;
        auto new_level = orders_at_price_pool_.allocate(order->side_, order->price_,
                                                        order, nullptr, nullptr);
        addOrdersAtPrice(new_level);
      } else {
        auto first = orders_at_price->first_mkt_order_;
        first->prev_order_->next_order_ = order;
        order->prev_order_              = first->prev_order_;
        order->next_order_              = first;
        first->prev_order_              = order;
      }

      oid_to_order_.at(order->order_id_) = order;
    }
  };

  typedef std::array<MarketOrderBook *, ME_MAX_TICKERS> MarketOrderBookHashMap;
}
