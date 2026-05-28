#include "strategy/trade_engine.h"

#include <cstdlib>

namespace {
  // Allow callers (e.g. backtests) to redirect the per-event engine log to
  // /dev/null. Production live binaries set nothing and get the original file.
  std::string engineLogPath(Common::ClientId client_id) {
    if (const char *e = std::getenv("TRADING_ENGINE_LOG_PATH"))
      return std::string(e);
    return "trading_engine_" + std::to_string(client_id) + ".log";
  }
}

namespace Trading {

  TradeEngine::TradeEngine(Common::ClientId client_id,
                           AlgoType algo_type,
                           const TradeEngineCfgHashMap &ticker_cfg,
                           Exchange::ClientRequestLFQueue  *client_requests,
                           Exchange::ClientResponseLFQueue *client_responses,
                           Exchange::MEMarketUpdateLFQueue *market_updates)
      : client_id_(client_id),
        outgoing_ogw_requests_(client_requests),
        incoming_ogw_responses_(client_responses),
        incoming_md_updates_(market_updates),
        logger_(engineLogPath(client_id)),
        // Step 1 — FeatureEngine reads smoothing knobs from cfg. Backtest
        // and showcase scripts always configure ticker 0; multi-ticker live
        // trading would need per-ticker FeatureEngines, deferred.
        feature_engine_(&logger_, ticker_cfg.at(0)),
        position_keeper_(&logger_, &ticker_cfg),
        risk_manager_(&logger_, &position_keeper_, ticker_cfg),
        order_manager_(&logger_, this, risk_manager_) {

    for (size_t i = 0; i < ticker_order_book_.size(); ++i) {
      ticker_order_book_[i] = new MarketOrderBook(i, &logger_);
      ticker_order_book_[i]->setTradeEngine(this);
    }

    // Day 4 — no more std::function lambda assignments. mm_algo_/taker_algo_
    // stay null until one of the branches below sets them. If neither is set
    // (e.g. RANDOM mode), the dispatch helpers in trade_engine.h are no-ops.

    // Push per-ticker hysteresis tolerances into OrderManager before any algo
    // construction so the algo callbacks see the right requote behaviour.
    for (TickerId i = 0; i < ticker_cfg.size(); ++i) {
      order_manager_.setHysteresisTicks(i, ticker_cfg.at(i).hysteresis_ticks_);
    }

    if (algo_type == AlgoType::MAKER) {
      mm_algo_ = new MarketMaker(&logger_, this, &feature_engine_,
                                 &position_keeper_,
                                 &order_manager_, ticker_cfg);
    } else if (algo_type == AlgoType::TAKER) {
      taker_algo_ = new LiquidityTaker(&logger_, this, &feature_engine_,
                                       &order_manager_, ticker_cfg);
    }

    for (TickerId i = 0; i < ticker_cfg.size(); ++i) {
      logger_.log("%:% %() % Initialized % Ticker:% %.\n",
                  __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_),
                  algoTypeToString(algo_type), i,
                  ticker_cfg.at(i).toString());
    }
  }

  TradeEngine::~TradeEngine() {
    run_ = false;
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);

    // Day 1 — dump per-tag latency histograms before destroying anything else.
    // Safe to call here: run() thread has exited (run_ = false + 1s sleep).
    dumpLatencyHistograms();

    delete mm_algo_;    mm_algo_    = nullptr;
    delete taker_algo_; taker_algo_ = nullptr;

    for (auto &order_book : ticker_order_book_) {
      delete order_book;
      order_book = nullptr;
    }
    outgoing_ogw_requests_  = nullptr;
    incoming_ogw_responses_ = nullptr;
    incoming_md_updates_    = nullptr;
  }

  auto TradeEngine::start() -> void {
    run_ = true;
    ASSERT(Common::createAndStartThread(-1, "Trading/TradeEngine",
                                         [this] { run(); }) != nullptr,
           "Failed to start TradeEngine thread.");
  }

  auto TradeEngine::stop() -> void {
    while (incoming_ogw_responses_->size() || incoming_md_updates_->size()) {
      logger_.log("%:% %() % Sleeping till all updates are consumed "
                  "ogw-size:% md-size:%\n",
                  __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_),
                  incoming_ogw_responses_->size(),
                  incoming_md_updates_->size());
      using namespace std::literals::chrono_literals;
      std::this_thread::sleep_for(10ms);
    }
    logger_.log("%:% %() % POSITIONS\n%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                position_keeper_.toString());
    run_ = false;
  }

  auto TradeEngine::sendClientRequest(const Exchange::MEClientRequest *client_request)
      noexcept -> void {
    logger_.log("%:% %() % Sending %\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                client_request->toString().c_str());
    auto next_write = outgoing_ogw_requests_->getNextToWriteTo();
    *next_write = std::move(*client_request);
    outgoing_ogw_requests_->updateWriteIndex();

    TTT_MEASURE(T10_TradeEngine_LFQueue_write, logger_);
  }

  auto TradeEngine::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
    while (run_) {
      for (auto client_response = incoming_ogw_responses_->getNextToRead();
           client_response;
           client_response = incoming_ogw_responses_->getNextToRead()) {
        TTT_MEASURE(T9t_TradeEngine_LFQueue_read, logger_);

        logger_.log("%:% %() % Processing %\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    client_response->toString().c_str());
        onOrderUpdate(client_response);
        incoming_ogw_responses_->updateReadIndex();
        last_event_time_ = Common::getCurrentNanos();
      }

      for (auto market_update = incoming_md_updates_->getNextToRead();
           market_update;
           market_update = incoming_md_updates_->getNextToRead()) {
        TTT_MEASURE(T9_TradeEngine_LFQueue_read, logger_);

        logger_.log("%:% %() % Processing %\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    market_update->toString().c_str());
        ASSERT(market_update->ticker_id_ < ticker_order_book_.size(),
               "Unknown ticker-id on update:" + market_update->toString());
        ticker_order_book_[market_update->ticker_id_]->
            onMarketUpdate(market_update);
        incoming_md_updates_->updateReadIndex();
        last_event_time_ = Common::getCurrentNanos();
      }
    }
  }

  auto TradeEngine::onOrderBookUpdate(TickerId ticker_id, Price price,
                                       Side side, MarketOrderBook *book)
      noexcept -> void {
    logger_.log("%:% %() % ticker:% price:% side:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                ticker_id, Common::priceToString(price).c_str(),
                Common::sideToString(side).c_str());

    const auto bbo = book->getBBO();

    START_MEASURE(Trading_PositionKeeper_updateBBO);
    position_keeper_.updateBBO(ticker_id, bbo);
    END_MEASURE_HIST(Trading_PositionKeeper_updateBBO, logger_, hist_pk_updateBBO_);

    START_MEASURE(Trading_FeatureEngine_onOrderBookUpdate);
    feature_engine_.onOrderBookUpdate(ticker_id, price, side, book);
    END_MEASURE_HIST(Trading_FeatureEngine_onOrderBookUpdate, logger_, hist_fe_obUpdate_);

    START_MEASURE(Trading_TradeEngine_algoOnOrderBookUpdate_);
    dispatchOnOrderBookUpdate(ticker_id, price, side, book);
    END_MEASURE_HIST(Trading_TradeEngine_algoOnOrderBookUpdate_, logger_, hist_algo_obUpdate_);
  }

  auto TradeEngine::onTradeUpdate(const Exchange::MEMarketUpdate *market_update,
                                   MarketOrderBook *book) noexcept -> void {
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                market_update->toString().c_str());

    START_MEASURE(Trading_FeatureEngine_onTradeUpdate);
    feature_engine_.onTradeUpdate(market_update, book);
    END_MEASURE_HIST(Trading_FeatureEngine_onTradeUpdate, logger_, hist_fe_trUpdate_);

    START_MEASURE(Trading_TradeEngine_algoOnTradeUpdate_);
    dispatchOnTradeUpdate(market_update, book);
    END_MEASURE_HIST(Trading_TradeEngine_algoOnTradeUpdate_, logger_, hist_algo_trUpdate_);
  }

  auto TradeEngine::onOrderUpdate(const Exchange::MEClientResponse *client_response)
      noexcept -> void {
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                client_response->toString().c_str());

    if (UNLIKELY(client_response->type_ == Exchange::ClientResponseType::FILLED)) {
      START_MEASURE(Trading_PositionKeeper_addFill);
      position_keeper_.addFill(client_response);
      END_MEASURE_HIST(Trading_PositionKeeper_addFill, logger_, hist_pk_addFill_);
    }

    START_MEASURE(Trading_TradeEngine_algoOnOrderUpdate_);
    dispatchOnOrderUpdate(client_response);
    END_MEASURE_HIST(Trading_TradeEngine_algoOnOrderUpdate_, logger_, hist_algo_ordUpdate_);
  }

  // Day 4 — defaultAlgo* removed. Dispatch now branches on mm_algo_/taker_algo_
  // directly; the RANDOM case has both null and falls through to a no-op.

  // ---------------------------------------------------------------------------
  // Day 1 — dump every per-tag latency histogram to ./latency_<client_id>_<tag>.hgrm
  // Filename includes client_id to avoid two trading_main instances clobbering
  // each other's files.
  // ---------------------------------------------------------------------------
  auto TradeEngine::dumpLatencyHistograms() const noexcept -> void {
    const Common::LatencyHistogram *const hists[] = {
        &hist_pk_updateBBO_,   &hist_fe_obUpdate_,   &hist_algo_obUpdate_,
        &hist_fe_trUpdate_,    &hist_algo_trUpdate_,
        &hist_pk_addFill_,     &hist_algo_ordUpdate_,
    };
    const auto prefix = "latency_" + std::to_string(client_id_) + "_";
    for (const auto *h : hists) {
      const auto path = prefix + h->name() + ".hgrm";
      h->dumpCsv(path);
    }
  }

} // namespace Trading
