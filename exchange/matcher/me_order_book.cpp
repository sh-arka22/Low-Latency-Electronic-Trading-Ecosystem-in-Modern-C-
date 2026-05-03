#include "matcher/me_order_book.h"

#include <cstdio>
#include <limits>

#include "matcher/matching_engine.h"

namespace Exchange {
  MEOrderBook::MEOrderBook(TickerId ticker_id, Logger *logger,
                           MatchingEngine *matching_engine)
      : ticker_id_(ticker_id),
        matching_engine_(matching_engine),
        orders_at_price_pool_(ME_MAX_PRICE_LEVELS),
        order_pool_(ME_MAX_ORDER_IDS),
        logger_(logger) {}

  MEOrderBook::~MEOrderBook() {
    logger_->log("%:% %() % OrderBook\n%\n", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_), toString(false, true));

    matching_engine_ = nullptr;
    bids_by_price_   = asks_by_price_ = nullptr;
    for (auto &row : cid_oid_to_order_) row.fill(nullptr);
  }

  // ----------------------------------------------------------------------
  // STEP 8 — single-fill execution between aggressor and one passive order.
  // ----------------------------------------------------------------------
  auto MEOrderBook::match(TickerId ticker_id, ClientId client_id, Side side,
                          OrderId client_order_id, OrderId new_market_order_id,
                          MEOrder *passive_order, Qty *leaves_qty) noexcept {
    const auto order_qty = passive_order->qty_;
    const auto fill_qty  = std::min(*leaves_qty, order_qty);

    *leaves_qty       -= fill_qty;
    passive_order->qty_ -= fill_qty;

    // Private FILLED to aggressor.
    client_response_ = {ClientResponseType::FILLED, client_id, ticker_id,
                        client_order_id, new_market_order_id, side,
                        passive_order->price_, fill_qty, *leaves_qty};
    matching_engine_->sendClientResponse(&client_response_);

    // Private FILLED to passive owner.
    client_response_ = {ClientResponseType::FILLED, passive_order->client_id_,
                        ticker_id, passive_order->client_order_id_,
                        passive_order->market_order_id_, passive_order->side_,
                        passive_order->price_, fill_qty, passive_order->qty_};
    matching_engine_->sendClientResponse(&client_response_);

    // Public TRADE print.
    market_update_ = {MarketUpdateType::TRADE, OrderId_INVALID, ticker_id, side,
                      passive_order->price_, fill_qty, Priority_INVALID};
    matching_engine_->sendMarketUpdate(&market_update_);

    // Passive order book update: remove if fully filled, else MODIFY remaining.
    if (!passive_order->qty_) {
      market_update_ = {MarketUpdateType::CANCEL, passive_order->market_order_id_,
                        ticker_id, passive_order->side_, passive_order->price_,
                        order_qty, Priority_INVALID};
      matching_engine_->sendMarketUpdate(&market_update_);
      removeOrder(passive_order);
    } else {
      market_update_ = {MarketUpdateType::MODIFY, passive_order->market_order_id_,
                        ticker_id, passive_order->side_, passive_order->price_,
                        passive_order->qty_, passive_order->priority_};
      matching_engine_->sendMarketUpdate(&market_update_);
    }
  }

  // ----------------------------------------------------------------------
  // STEP 8 — greedy walk down the opposite side while prices cross.
  // ----------------------------------------------------------------------
  auto MEOrderBook::checkForMatch(ClientId client_id, OrderId client_order_id,
                                  TickerId ticker_id, Side side, Price price,
                                  Qty qty, Qty new_market_order_id) noexcept {
    auto leaves_qty = qty;

    if (side == Side::BUY) {
      while (leaves_qty && asks_by_price_) {
        const auto best_ask = asks_by_price_->first_me_order_;
        if (LIKELY(price < best_ask->price_)) break;  // no more crosses
        match(ticker_id, client_id, side, client_order_id, new_market_order_id,
              best_ask, &leaves_qty);
      }
    }
    if (side == Side::SELL) {
      while (leaves_qty && bids_by_price_) {
        const auto best_bid = bids_by_price_->first_me_order_;
        if (LIKELY(price > best_bid->price_)) break;
        match(ticker_id, client_id, side, client_order_id, new_market_order_id,
              best_bid, &leaves_qty);
      }
    }
    return leaves_qty;
  }

  // ----------------------------------------------------------------------
  // STEP 6 — accept order, attempt match, rest residual on the book.
  // ----------------------------------------------------------------------
  auto MEOrderBook::add(ClientId client_id, OrderId client_order_id,
                        TickerId ticker_id, Side side, Price price,
                        Qty qty) noexcept -> void {
    const auto new_market_order_id = generateNewMarketOrderId();

    client_response_ = {ClientResponseType::ACCEPTED, client_id, ticker_id,
                        client_order_id, new_market_order_id, side, price, 0, qty};
    matching_engine_->sendClientResponse(&client_response_);

    const auto leaves_qty = checkForMatch(client_id, client_order_id, ticker_id,
                                          side, price, qty, new_market_order_id);

    if (LIKELY(leaves_qty)) {
      const auto priority = getNextPriority(price);

      auto order = order_pool_.allocate(ticker_id, client_id, client_order_id,
                                        new_market_order_id, side, price,
                                        leaves_qty, priority, nullptr, nullptr);
      addOrder(order);

      market_update_ = {MarketUpdateType::ADD, new_market_order_id, ticker_id,
                        side, price, leaves_qty, priority};
      matching_engine_->sendMarketUpdate(&market_update_);
    }
  }

  // ----------------------------------------------------------------------
  // STEP 7 — cancel a resting order, or reject if not found.
  // ----------------------------------------------------------------------
  auto MEOrderBook::cancel(ClientId client_id, OrderId order_id,
                           TickerId ticker_id) noexcept -> void {
    auto     is_cancelable = (client_id < cid_oid_to_order_.size());
    MEOrder *exchange_order = nullptr;
    if (LIKELY(is_cancelable)) {
      auto &row      = cid_oid_to_order_.at(client_id);
      exchange_order = row.at(order_id);
      is_cancelable  = (exchange_order != nullptr);
    }

    if (UNLIKELY(!is_cancelable)) {
      client_response_ = {ClientResponseType::CANCEL_REJECTED, client_id,
                          ticker_id, order_id, OrderId_INVALID, Side::INVALID,
                          Price_INVALID, Qty_INVALID, Qty_INVALID};
    } else {
      client_response_ = {ClientResponseType::CANCELED, client_id, ticker_id,
                          order_id, exchange_order->market_order_id_,
                          exchange_order->side_, exchange_order->price_,
                          Qty_INVALID, exchange_order->qty_};
      market_update_   = {MarketUpdateType::CANCEL, exchange_order->market_order_id_,
                          ticker_id, exchange_order->side_, exchange_order->price_,
                          0, exchange_order->priority_};

      removeOrder(exchange_order);
      matching_engine_->sendMarketUpdate(&market_update_);
    }

    matching_engine_->sendClientResponse(&client_response_);
  }

  // ----------------------------------------------------------------------
  // Diagnostic dump of both sides; optional sanity check that levels are sorted.
  // ----------------------------------------------------------------------
  auto MEOrderBook::toString(bool detailed, bool validity_check) const -> std::string {
    std::stringstream ss;

    auto printer = [&](std::stringstream &ss, MEOrdersAtPrice *itr, Side side,
                       Price &last_price, bool sanity_check) {
      char   buf[4096];
      Qty    qty        = 0;
      size_t num_orders = 0;

      for (auto o = itr->first_me_order_;; o = o->next_order_) {
        qty += o->qty_;
        ++num_orders;
        if (o->next_order_ == itr->first_me_order_) break;
      }

      snprintf(buf, sizeof(buf), " <px:%3s p:%3s n:%3s> %-3s @ %-5s(%-4s)",
               priceToString(itr->price_).c_str(),
               priceToString(itr->prev_entry_->price_).c_str(),
               priceToString(itr->next_entry_->price_).c_str(),
               priceToString(itr->price_).c_str(),
               qtyToString(qty).c_str(),
               std::to_string(num_orders).c_str());
      ss << buf;

      for (auto o = itr->first_me_order_;; o = o->next_order_) {
        if (detailed) {
          snprintf(buf, sizeof(buf), "[oid:%s q:%s p:%s n:%s] ",
                   orderIdToString(o->market_order_id_).c_str(),
                   qtyToString(o->qty_).c_str(),
                   orderIdToString(o->prev_order_ ? o->prev_order_->market_order_id_ : OrderId_INVALID).c_str(),
                   orderIdToString(o->next_order_ ? o->next_order_->market_order_id_ : OrderId_INVALID).c_str());
          ss << buf;
        }
        if (o->next_order_ == itr->first_me_order_) break;
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
    return ss.str();
  }
}
