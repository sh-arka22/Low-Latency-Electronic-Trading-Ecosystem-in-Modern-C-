#pragma once

#include <functional>

#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"
#include "common/logging.h"

#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "exchange/market_data/market_update.h"

#include "strategy/market_order_book.h"
#include "strategy/feature_engine.h"
#include "strategy/position_keeper.h"
#include "strategy/order_manager.h"
#include "strategy/risk_manager.h"
#include "strategy/market_maker.h"
#include "strategy/liquidity_taker.h"

namespace Trading {

  class TradeEngine {
  public:
    TradeEngine(Common::ClientId client_id,
                AlgoType algo_type,
                const TradeEngineCfgHashMap &ticker_cfg,
                Exchange::ClientRequestLFQueue  *client_requests,
                Exchange::ClientResponseLFQueue *client_responses,
                Exchange::MEMarketUpdateLFQueue *market_updates);

    ~TradeEngine();

    auto start() -> void;
    auto stop()  -> void;

    auto sendClientRequest(const Exchange::MEClientRequest *client_request)
        noexcept -> void;

    auto onOrderBookUpdate(TickerId ticker_id, Price price,
                           Side side, MarketOrderBook *book) noexcept -> void;
    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update,
                       MarketOrderBook *book) noexcept -> void;
    auto onOrderUpdate(const Exchange::MEClientResponse *client_response)
        noexcept -> void;

    auto initLastEventTime() { last_event_time_ = Common::getCurrentNanos(); }
    auto silentSeconds()     { return (Common::getCurrentNanos() - last_event_time_)
                                       / NANOS_TO_SECS; }
    auto clientId() const    { return client_id_; }

    // Public so MarketMaker / LiquidityTaker constructors can overwrite them.
    std::function<void(TickerId, Price, Side, MarketOrderBook *)>
        algoOnOrderBookUpdate_;
    std::function<void(const Exchange::MEMarketUpdate *, MarketOrderBook *)>
        algoOnTradeUpdate_;
    std::function<void(const Exchange::MEClientResponse *)>
        algoOnOrderUpdate_;

    TradeEngine()                                = delete;
    TradeEngine(const TradeEngine &)             = delete;
    TradeEngine(const TradeEngine &&)            = delete;
    TradeEngine &operator=(const TradeEngine &)  = delete;
    TradeEngine &operator=(const TradeEngine &&) = delete;

  private:
    const ClientId client_id_;

    Exchange::ClientRequestLFQueue  *outgoing_ogw_requests_  = nullptr;
    Exchange::ClientResponseLFQueue *incoming_ogw_responses_ = nullptr;
    Exchange::MEMarketUpdateLFQueue *incoming_md_updates_    = nullptr;

    Nanos          last_event_time_ = 0;
    volatile bool  run_             = false;
    std::string    time_str_;
    Logger         logger_;

    MarketOrderBookHashMap ticker_order_book_;

    // declaration order matters: risk_manager_ must precede order_manager_
    // because OrderManager's constructor binds a reference to risk_manager_.
    FeatureEngine   feature_engine_;
    PositionKeeper  position_keeper_;
    RiskManager     risk_manager_;
    OrderManager    order_manager_;

    MarketMaker    *mm_algo_    = nullptr;
    LiquidityTaker *taker_algo_ = nullptr;

    auto run() noexcept -> void;

    auto defaultAlgoOnOrderBookUpdate(TickerId ticker_id, Price price,
                                      Side side, MarketOrderBook *) noexcept -> void;
    auto defaultAlgoOnTradeUpdate(const Exchange::MEMarketUpdate *market_update,
                                  MarketOrderBook *) noexcept -> void;
    auto defaultAlgoOnOrderUpdate(const Exchange::MEClientResponse *client_response)
        noexcept -> void;
  };
}
