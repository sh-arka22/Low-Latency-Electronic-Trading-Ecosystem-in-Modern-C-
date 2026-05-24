#include "backtest/backtest_engine.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace Backtest {

  using Common::Price;
  using Common::Qty;
  using Common::Side;
  using Common::Nanos;
  using Common::OrderId;
  using Common::TickerId;
  using Common::sideToIndex;

  BacktestEngine::BacktestEngine(Config cfg)
      : cfg_(std::move(cfg)),
        logger_("backtest_" + cfg_.strategy_label + ".log"),
        client_requests_(Common::ME_MAX_CLIENT_UPDATES),
        client_responses_(Common::ME_MAX_CLIENT_UPDATES),
        market_updates_(Common::ME_MAX_MARKET_UPDATES) {

    // Build TradeEngine cfg map — only the configured ticker uses our cfg,
    // every other slot stays default (clip_=0 → algo never quotes them).
    Common::TradeEngineCfgHashMap ticker_cfg{};
    ticker_cfg.at(cfg_.ticker_id) = cfg_.cfg;

    engine_ = std::make_unique<Trading::TradeEngine>(
        /*client_id=*/1,
        Common::AlgoType::MAKER,
        ticker_cfg,
        &client_requests_, &client_responses_, &market_updates_);

    // We do NOT call engine_->start() — the harness drives the engine
    // synchronously by calling its onXxx methods directly. The engine's
    // internal thread loop would race with our replay clock.

    tape_ = std::make_unique<TapeReader>(cfg_.tape);

    for (auto &row : live_)
      for (auto &o : row)
        o = {};

    pnl_out_.open(cfg_.pnl_csv_path);
    pnl_out_ << "ts_ns,mid,bid,ask,position,real_pnl,unreal_pnl,total_pnl,volume,"
             << "num_fills,num_requotes,sigma,ofi\n";
  }

  BacktestEngine::~BacktestEngine() {
    if (pnl_out_.is_open()) pnl_out_.close();
  }

  auto BacktestEngine::run() -> void {
    Nanos last_pnl_ns = 0;
    static constexpr Nanos kPnlSamplePeriodNs = 50'000'000;   // every 50 ms

    while (auto ev = tape_->next()) {
      Nanos now_ns = 0;
      if (std::holds_alternative<BookTopEvent>(*ev)) {
        const auto &b = std::get<BookTopEvent>(*ev);
        now_ns = b.event_ns;
        applyBookEvent(b);
      } else {
        const auto &t = std::get<TradeEvent>(*ev);
        now_ns = t.event_ns;
        applyTradeEvent(t);
      }

      pumpClientRequests(now_ns);
      pumpClientResponses();

      if (now_ns - last_pnl_ns >= kPnlSamplePeriodNs) {
        recordPnL(now_ns);
        last_pnl_ns = now_ns;
      }
    }

    // Drain.
    pumpClientResponses();
    recordPnL(last_pnl_ns);
  }

  // --------------------------------------------------------------------
  // Apply a top-of-book update by CANCEL/ADD on synthetic resting orders.
  //
  // Cancels must precede adds across BOTH sides — MarketOrderBook indexes price
  // levels by (price % ME_MAX_PRICE_LEVELS) without checking Side, so if the
  // new bid price equals the soon-to-be-canceled ask price (a tick crossover
  // when mid moves through the spread), addOrder finds the existing ask level
  // and appends a BUY into a SELL chain → silent book corruption that explodes
  // later in addOrdersAtPrice when next_entry_ is null.
  // --------------------------------------------------------------------
  auto BacktestEngine::applyBookEvent(const BookTopEvent &ev) -> void {
    const auto tid = cfg_.ticker_id;

    const bool bid_changed = (ev.bid_price != last_bid_price_ || ev.bid_qty != last_bid_qty_);
    const bool ask_changed = (ev.ask_price != last_ask_price_ || ev.ask_qty != last_ask_qty_);

    // 1) Cancel both sides up-front.
    if (bid_changed && resting_bid_oid_ != Common::OrderId_INVALID) {
      Exchange::MEMarketUpdate cxl{
          Exchange::MarketUpdateType::CANCEL,
          resting_bid_oid_, tid, Side::BUY, last_bid_price_,
          last_bid_qty_, 0};
      publishMarketUpdate(cxl);
      resting_bid_oid_ = Common::OrderId_INVALID;
    }
    if (ask_changed && resting_ask_oid_ != Common::OrderId_INVALID) {
      Exchange::MEMarketUpdate cxl{
          Exchange::MarketUpdateType::CANCEL,
          resting_ask_oid_, tid, Side::SELL, last_ask_price_,
          last_ask_qty_, 0};
      publishMarketUpdate(cxl);
      resting_ask_oid_ = Common::OrderId_INVALID;
    }

    // 2) Add both sides.
    if (bid_changed) {
      if (ev.bid_price != Common::Price_INVALID) {
        resting_bid_oid_ = nextSynthOid();
        Exchange::MEMarketUpdate add{
            Exchange::MarketUpdateType::ADD,
            resting_bid_oid_, tid, Side::BUY, ev.bid_price,
            ev.bid_qty, 0};
        publishMarketUpdate(add);
      }
      last_bid_price_ = ev.bid_price;
      last_bid_qty_   = ev.bid_qty;
    }
    if (ask_changed) {
      if (ev.ask_price != Common::Price_INVALID) {
        resting_ask_oid_ = nextSynthOid();
        Exchange::MEMarketUpdate add{
            Exchange::MarketUpdateType::ADD,
            resting_ask_oid_, tid, Side::SELL, ev.ask_price,
            ev.ask_qty, 0};
        publishMarketUpdate(add);
      }
      last_ask_price_ = ev.ask_price;
      last_ask_qty_   = ev.ask_qty;
    }

    matchAgainstBBO(ev.event_ns);
  }

  auto BacktestEngine::applyTradeEvent(const TradeEvent &ev) -> void {
    Exchange::MEMarketUpdate trade{
        Exchange::MarketUpdateType::TRADE,
        Common::OrderId_INVALID, cfg_.ticker_id, ev.side,
        ev.price, ev.qty, 0};
    publishMarketUpdate(trade);
    matchAgainstTrade(ev);
  }

  // --------------------------------------------------------------------
  // Push an event into the market-update LFQueue and have the TradeEngine
  // pop it synchronously — this fires book update + algo callbacks.
  // --------------------------------------------------------------------
  auto BacktestEngine::publishMarketUpdate(const Exchange::MEMarketUpdate &mu) -> void {
    auto *book = engine_->orderBook(mu.ticker_id_);
    book->onMarketUpdate(&mu);
  }

  auto BacktestEngine::publishResponse(const Exchange::MEClientResponse &cr) -> void {
    auto slot = client_responses_.getNextToWriteTo();
    *slot = cr;
    client_responses_.updateWriteIndex();
  }

  // --------------------------------------------------------------------
  // Drain client_requests_ (new/cancel emitted by OrderManager) and apply
  // them to our local "live order" tracker. ACCEPTED / CANCELED responses
  // are pushed back so the TradeEngine state machine stays in sync.
  // --------------------------------------------------------------------
  auto BacktestEngine::pumpClientRequests(Nanos now_ns) -> void {
    for (auto req = client_requests_.getNextToRead(); req;
         req = client_requests_.getNextToRead()) {
      const auto tid   = req->ticker_id_;
      const auto side  = req->side_;
      const auto idx   = sideToIndex(side);
      auto      &slot  = live_.at(tid).at(idx);

      Exchange::MEClientResponse rsp{};
      rsp.client_id_       = req->client_id_;
      rsp.ticker_id_       = tid;
      rsp.client_order_id_ = req->order_id_;
      rsp.market_order_id_ = req->order_id_;
      rsp.side_            = side;
      rsp.price_           = req->price_;
      rsp.exec_qty_        = 0;
      rsp.leaves_qty_      = req->qty_;

      if (req->type_ == Exchange::ClientRequestType::NEW) {
        slot.order_id   = req->order_id_;
        slot.price      = req->price_;
        slot.side       = side;
        slot.leaves     = req->qty_;
        slot.created_ns = now_ns;
        // queue ahead estimate: depth currently at our price level (assume we
        // join the back of the visible queue at top-of-book).
        // Optimistic queue model: assume we get to the front of the queue at
        // insert time. This makes fills sensitive primarily to *how long* the
        // order stays live (i.e. hysteresis vs requote-every-tick) which is
        // the dominant variable we're trying to compare across strategies.
        slot.ahead = 0;
        (void)last_bid_qty_; (void)last_ask_qty_;
        ++num_requotes_;
        rsp.type_ = Exchange::ClientResponseType::ACCEPTED;
      } else {
        // CANCEL
        rsp.type_   = Exchange::ClientResponseType::CANCELED;
        slot        = {};
      }
      publishResponse(rsp);
      client_requests_.updateReadIndex();
    }
  }

  auto BacktestEngine::pumpClientResponses() -> void {
    for (auto rsp = client_responses_.getNextToRead(); rsp;
         rsp = client_responses_.getNextToRead()) {
      engine_->onOrderUpdate(rsp);
      client_responses_.updateReadIndex();
    }
  }

  // --------------------------------------------------------------------
  // Fills.
  // (a) BBO-crossing: if our bid is at or above the new ask, fill us.
  //                   if our ask is at or below the new bid, fill us.
  // (b) Trade burns down ahead_qty; if ahead drops to 0 and a trade prints
  //     at our price on the opposite side, we get filled.
  // --------------------------------------------------------------------
  auto BacktestEngine::matchAgainstBBO(Nanos now_ns) -> void {
    const auto tid = cfg_.ticker_id;
    auto       &bid_slot = live_.at(tid).at(sideToIndex(Side::BUY));
    auto       &ask_slot = live_.at(tid).at(sideToIndex(Side::SELL));

    if (bid_slot.leaves > 0
        && last_ask_price_ != Common::Price_INVALID
        && bid_slot.price >= last_ask_price_) {
      const auto fill_qty = std::min<Qty>(bid_slot.leaves, last_ask_qty_);
      Exchange::MEClientResponse rsp{};
      rsp.type_            = Exchange::ClientResponseType::FILLED;
      rsp.client_id_       = 1;
      rsp.ticker_id_       = tid;
      rsp.client_order_id_ = bid_slot.order_id;
      rsp.market_order_id_ = bid_slot.order_id;
      rsp.side_            = Side::BUY;
      rsp.price_           = last_ask_price_;  // execution at the touch
      rsp.exec_qty_        = fill_qty;
      rsp.leaves_qty_      = bid_slot.leaves - fill_qty;
      bid_slot.leaves      = rsp.leaves_qty_;
      if (!bid_slot.leaves) bid_slot = {};
      ++num_fills_;
      publishResponse(rsp);
    }

    if (ask_slot.leaves > 0
        && last_bid_price_ != Common::Price_INVALID
        && ask_slot.price <= last_bid_price_) {
      const auto fill_qty = std::min<Qty>(ask_slot.leaves, last_bid_qty_);
      Exchange::MEClientResponse rsp{};
      rsp.type_            = Exchange::ClientResponseType::FILLED;
      rsp.client_id_       = 1;
      rsp.ticker_id_       = tid;
      rsp.client_order_id_ = ask_slot.order_id;
      rsp.market_order_id_ = ask_slot.order_id;
      rsp.side_            = Side::SELL;
      rsp.price_           = last_bid_price_;
      rsp.exec_qty_        = fill_qty;
      rsp.leaves_qty_      = ask_slot.leaves - fill_qty;
      ask_slot.leaves      = rsp.leaves_qty_;
      if (!ask_slot.leaves) ask_slot = {};
      ++num_fills_;
      publishResponse(rsp);
    }
    (void)now_ns;
  }

  auto BacktestEngine::matchAgainstTrade(const TradeEvent &ev) -> void {
    const auto tid = cfg_.ticker_id;
    if (ev.side == Side::BUY) {
      auto &ask_slot = live_.at(tid).at(sideToIndex(Side::SELL));
      if (ask_slot.leaves > 0 && ev.price >= ask_slot.price) {
        // Drain queue first.
        const auto eat = std::min<double>(ask_slot.ahead, ev.qty);
        ask_slot.ahead -= static_cast<Qty>(eat);
        const auto remaining = static_cast<Qty>(ev.qty - eat);
        if (remaining > 0 && ask_slot.ahead == 0) {
          const auto fill_qty = std::min<Qty>(ask_slot.leaves, remaining);
          Exchange::MEClientResponse rsp{};
          rsp.type_            = Exchange::ClientResponseType::FILLED;
          rsp.client_id_       = 1;
          rsp.ticker_id_       = tid;
          rsp.client_order_id_ = ask_slot.order_id;
          rsp.market_order_id_ = ask_slot.order_id;
          rsp.side_            = Side::SELL;
          rsp.price_           = ask_slot.price;
          rsp.exec_qty_        = fill_qty;
          rsp.leaves_qty_      = ask_slot.leaves - fill_qty;
          ask_slot.leaves      = rsp.leaves_qty_;
          if (!ask_slot.leaves) ask_slot = {};
          ++num_fills_;
          publishResponse(rsp);
        }
      }
    } else if (ev.side == Side::SELL) {
      auto &bid_slot = live_.at(tid).at(sideToIndex(Side::BUY));
      if (bid_slot.leaves > 0 && ev.price <= bid_slot.price) {
        const auto eat = std::min<double>(bid_slot.ahead, ev.qty);
        bid_slot.ahead -= static_cast<Qty>(eat);
        const auto remaining = static_cast<Qty>(ev.qty - eat);
        if (remaining > 0 && bid_slot.ahead == 0) {
          const auto fill_qty = std::min<Qty>(bid_slot.leaves, remaining);
          Exchange::MEClientResponse rsp{};
          rsp.type_            = Exchange::ClientResponseType::FILLED;
          rsp.client_id_       = 1;
          rsp.ticker_id_       = tid;
          rsp.client_order_id_ = bid_slot.order_id;
          rsp.market_order_id_ = bid_slot.order_id;
          rsp.side_            = Side::BUY;
          rsp.price_           = bid_slot.price;
          rsp.exec_qty_        = fill_qty;
          rsp.leaves_qty_      = bid_slot.leaves - fill_qty;
          bid_slot.leaves      = rsp.leaves_qty_;
          if (!bid_slot.leaves) bid_slot = {};
          ++num_fills_;
          publishResponse(rsp);
        }
      }
    }
  }

  auto BacktestEngine::recordPnL(Nanos now_ns) -> void {
    const auto pos_info = engine_->positionInfo(cfg_.ticker_id);
    const auto mid = (last_bid_price_ != Common::Price_INVALID &&
                      last_ask_price_ != Common::Price_INVALID)
                  ? 0.5 * (last_bid_price_ + last_ask_price_)
                  : 0.0;

    pnl_out_ << now_ns << ','
             << std::fixed << std::setprecision(2) << mid << ','
             << last_bid_price_ << ','
             << last_ask_price_ << ','
             << pos_info.position << ','
             << pos_info.real_pnl << ','
             << pos_info.unreal_pnl << ','
             << pos_info.total_pnl << ','
             << pos_info.volume << ','
             << num_fills_ << ','
             << num_requotes_ << ','
             << engine_->volatility() << ','
             << engine_->ofi()
             << '\n';
    ++pnl_row_count_;
  }
}
