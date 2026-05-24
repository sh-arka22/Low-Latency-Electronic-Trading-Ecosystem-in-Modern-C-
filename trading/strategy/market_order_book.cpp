#include "strategy/market_order_book.h"

#include <cstdio>
#include <limits>

#include "strategy/trade_engine.h"

namespace Trading {
  MarketOrderBook::MarketOrderBook(TickerId ticker_id, Logger *logger)
      : ticker_id_(ticker_id),
        orders_at_price_pool_(ME_MAX_PRICE_LEVELS),
        order_pool_(ME_MAX_ORDER_IDS),
        logger_(logger) {}

  MarketOrderBook::~MarketOrderBook() {
    logger_->log("%:% %() % OrderBook\n%\n", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_), toString(false, true));
    trade_engine_ = nullptr;
    bids_by_price_ = asks_by_price_ = nullptr;
    oid_to_order_.fill(nullptr);
  }

  // --------------------------------------------------------------------
  // Apply one market update to the client view of the book and notify
  // the strategy via TradeEngine callbacks.
  // --------------------------------------------------------------------
  auto MarketOrderBook::onMarketUpdate(const Exchange::MEMarketUpdate *market_update) noexcept -> void {
    // Decide up-front whether this event can affect the best price level on
    // either side. We need this before mutating the book.
    //
    // v1.1 fix: the original predicate "bids_by_price_ && price >= best.price"
    // was false on the *first* ADD because bids_by_price_ was still null —
    // updateBBO then skipped and the BBO stayed at Price_INVALID forever,
    // so any strategy keying off BBO never quoted. Treat an empty side as
    // an implicit BBO change for any ADD on that side.
    const auto bid_updated = market_update->side_ == Side::BUY &&
                             (bids_by_price_ == nullptr ||
                              market_update->price_ >= bids_by_price_->price_);
    const auto ask_updated = market_update->side_ == Side::SELL &&
                             (asks_by_price_ == nullptr ||
                              market_update->price_ <= asks_by_price_->price_);

    switch (market_update->type_) {
      case Exchange::MarketUpdateType::ADD: {
        auto order = order_pool_.allocate(market_update->order_id_,
                                          market_update->side_,
                                          market_update->price_,
                                          market_update->qty_,
                                          market_update->priority_,
                                          nullptr, nullptr);
        addOrder(order);
      } break;

      case Exchange::MarketUpdateType::MODIFY: {
        auto order = oid_to_order_.at(market_update->order_id_);
        order->qty_ = market_update->qty_;
      } break;

      case Exchange::MarketUpdateType::CANCEL: {
        auto order = oid_to_order_.at(market_update->order_id_);
        removeOrder(order);
      } break;

      case Exchange::MarketUpdateType::TRADE: {
        // Trades do not alter the book — matching engine has already published
        // CANCEL/MODIFY for the resting order. Notify the strategy and exit
        // before updateBBO / onOrderBookUpdate run.
        trade_engine_->onTradeUpdate(market_update, this);
        return;
      }

      case Exchange::MarketUpdateType::CLEAR: {
        // Snapshot recovery is starting — wipe everything for this ticker.
        for (auto &order : oid_to_order_) {
          if (order) order_pool_.deallocate(order);
        }
        oid_to_order_.fill(nullptr);

        if (bids_by_price_) {
          for (auto bid = bids_by_price_->next_entry_;
               bid != bids_by_price_;
               bid = bid->next_entry_) {
            orders_at_price_pool_.deallocate(bid);
          }
          orders_at_price_pool_.deallocate(bids_by_price_);
        }
        if (asks_by_price_) {
          for (auto ask = asks_by_price_->next_entry_;
               ask != asks_by_price_;
               ask = ask->next_entry_) {
            orders_at_price_pool_.deallocate(ask);
          }
          orders_at_price_pool_.deallocate(asks_by_price_);
        }
        bids_by_price_ = asks_by_price_ = nullptr;
      } break;

      case Exchange::MarketUpdateType::INVALID:
      case Exchange::MarketUpdateType::SNAPSHOT_START:
      case Exchange::MarketUpdateType::SNAPSHOT_END:
        break;
    }

    updateBBO(bid_updated, ask_updated);

    trade_engine_->onOrderBookUpdate(market_update->ticker_id_,
                                     market_update->price_,
                                     market_update->side_, this);
  }

  // --------------------------------------------------------------------
  // Diagnostic dump (mirrors MEOrderBook::toString).
  // --------------------------------------------------------------------
  auto MarketOrderBook::toString(bool detailed, bool validity_check) const -> std::string {
    std::stringstream ss;

    auto printer = [&](std::stringstream &ss, MarketOrdersAtPrice *itr, Side side,
                       Price &last_price, bool sanity_check) {
      char   buf[4096];
      Qty    qty        = 0;
      size_t num_orders = 0;

      for (auto o = itr->first_mkt_order_;; o = o->next_order_) {
        qty += o->qty_;
        ++num_orders;
        if (o->next_order_ == itr->first_mkt_order_) break;
      }

      snprintf(buf, sizeof(buf), " <px:%3s p:%3s n:%3s> %-3s @ %-5s(%-4s)",
               priceToString(itr->price_).c_str(),
               priceToString(itr->prev_entry_->price_).c_str(),
               priceToString(itr->next_entry_->price_).c_str(),
               priceToString(itr->price_).c_str(),
               qtyToString(qty).c_str(),
               std::to_string(num_orders).c_str());
      ss << buf;

      for (auto o = itr->first_mkt_order_;; o = o->next_order_) {
        if (detailed) {
          snprintf(buf, sizeof(buf), "[oid:%s q:%s p:%s n:%s] ",
                   orderIdToString(o->order_id_).c_str(),
                   qtyToString(o->qty_).c_str(),
                   orderIdToString(o->prev_order_ ? o->prev_order_->order_id_ : OrderId_INVALID).c_str(),
                   orderIdToString(o->next_order_ ? o->next_order_->order_id_ : OrderId_INVALID).c_str());
          ss << buf;
        }
        if (o->next_order_ == itr->first_mkt_order_) break;
      }
      ss << std::endl;

      if (sanity_check) {
        if ((side == Side::SELL && last_price >= itr->price_) ||
            (side == Side::BUY  && last_price <= itr->price_)) {
          FATAL("Bids/Asks not sorted last:" + priceToString(last_price) +
                " itr:" + itr->toString());
        }
        last_price = itr->price_;
      }
    };

    ss << "Ticker:" << tickerIdToString(ticker_id_) << std::endl;
    {
      auto itr            = asks_by_price_;
      auto last_ask_price = std::numeric_limits<Price>::min();
      for (size_t count = 0; itr; ++count) {
        ss << "ASKS L:" << count << " => ";
        auto next = (itr->next_entry_ == asks_by_price_ ? nullptr : itr->next_entry_);
        printer(ss, itr, Side::SELL, last_ask_price, validity_check);
        itr = next;
      }
    }

    ss << std::endl << "                          X" << std::endl << std::endl;

    {
      auto itr            = bids_by_price_;
      auto last_bid_price = std::numeric_limits<Price>::max();
      for (size_t count = 0; itr; ++count) {
        ss << "BIDS L:" << count << " => ";
        auto next = (itr->next_entry_ == bids_by_price_ ? nullptr : itr->next_entry_);
        printer(ss, itr, Side::BUY, last_bid_price, validity_check);
        itr = next;
      }
    }
    ss << "\n" << bbo_.toString() << "\n";
    return ss.str();
  }
}
