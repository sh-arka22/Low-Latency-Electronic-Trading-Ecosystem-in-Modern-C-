#include <algorithm>
#include <cmath>

#include "strategy/market_maker.h"
#include "strategy/trade_engine.h"

namespace Trading {

  MarketMaker::MarketMaker(Common::Logger *logger,
                           TradeEngine * /*trade_engine*/,
                           const FeatureEngine *feature_engine,
                           const PositionKeeper *position_keeper,
                           OrderManager *order_manager,
                           const TradeEngineCfgHashMap &ticker_cfg)
      : feature_engine_(feature_engine),
        position_keeper_(position_keeper),
        order_manager_(order_manager),
        logger_(logger),
        ticker_cfg_(ticker_cfg),
        session_start_ns_(Common::getCurrentNanos()) {
    // Day 4 — algoOn*_ std::function writes removed. TradeEngine now holds the
    // mm_algo_ pointer to this instance and calls our on*Update methods directly
    // via inlined dispatch helpers (see trade_engine.h).
  }

  auto MarketMaker::onOrderBookUpdate(TickerId ticker_id, Price price,
                                       Side side, const MarketOrderBook *book)
      noexcept -> void {
    logger_->log("%:% %() % ticker:% price:% side:%\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 ticker_id, Common::priceToString(price).c_str(),
                 Common::sideToString(side).c_str());

    const auto bbo        = book->getBBO();
    const auto fair_price = feature_engine_->getMktPrice();

    if (LIKELY(bbo->bid_price_ != Price_INVALID &&
               bbo->ask_price_ != Price_INVALID &&
               fair_price      != Feature_INVALID)) {

      const auto &cfg       = ticker_cfg_.at(ticker_id);
      const auto  clip_base = cfg.clip_;
      const auto  threshold = cfg.threshold_;
      const auto  sigma     = feature_engine_->getVolatility();

      Price bid_price = Price_INVALID;
      Price ask_price = Price_INVALID;
      Qty   clip      = clip_base;

      // AS path: requires σ to be initialised and use_as_ enabled in cfg.
      const bool as_ready = cfg.use_as_
                         && sigma != Feature_INVALID
                         && sigma > 0.0;

      if (as_ready) {
        const auto  q       = static_cast<double>(
            position_keeper_->getPositionInfo(ticker_id)->position_);
        const auto  gamma   = cfg.gamma_;
        const auto  kappa   = std::max(1e-6, cfg.kappa_);

        // τ = remaining fraction of session, floored to avoid degeneracy.
        const auto  elapsed_ns = Common::getCurrentNanos() - session_start_ns_;
        const auto  session_ns = static_cast<double>(cfg.session_hours_) * 3600.0 * 1e9;
        const auto  tau        = std::max(1e-3,
            (session_ns - static_cast<double>(elapsed_ns)) / session_ns);

        // Avellaneda-Stoikov reservation price + optimal spread.
        double reservation = fair_price - q * gamma * sigma * sigma * tau;
        if (cfg.use_ofi_)
          reservation += cfg.beta_ofi_ * feature_engine_->getOFI();

        const double spread = gamma * sigma * sigma * tau
                            + (2.0 / gamma) * std::log1p(gamma / kappa);

        bid_price = static_cast<Price>(std::floor(reservation - 0.5 * spread));
        ask_price = static_cast<Price>(std::ceil (reservation + 0.5 * spread));

        // Don't let our quote cross the book (would consume liquidity).
        if (bid_price >= bbo->ask_price_) bid_price = bbo->ask_price_ - 1;
        if (ask_price <= bbo->bid_price_) ask_price = bbo->bid_price_ + 1;

        // Adaptive clip — shrink size near inventory limits and in high σ.
        if (cfg.use_adaptive_clip_ && cfg.risk_cfg_.max_position_ > 0) {
          const auto inv_room = std::max(0.0, 1.0 -
              std::abs(q) / static_cast<double>(cfg.risk_cfg_.max_position_));
          const auto sigma_scale = std::clamp(cfg.sigma_ref_ / sigma, 0.5, 1.5);
          const auto scaled = std::round(static_cast<double>(clip_base)
                                          * inv_room * sigma_scale);
          clip = static_cast<Qty>(std::max(1.0, scaled));
        }

        logger_->log("%:% %() % AS ticker:% q:% σ:% τ:% ofi:% res:% spr:% bid:% ask:% clip:%\n",
                     __FILE__, __LINE__, __FUNCTION__,
                     Common::getCurrentTimeStr(&time_str_),
                     ticker_id, q, sigma, tau,
                     feature_engine_->getOFI(),
                     reservation, spread, bid_price, ask_price, clip);
      } else {
        // v1.0 threshold-pennying fallback (warmup or use_as_ disabled).
        bid_price = bbo->bid_price_ -
            (fair_price - bbo->bid_price_ >= threshold ? 0 : 1);
        ask_price = bbo->ask_price_ +
            (bbo->ask_price_ - fair_price >= threshold ? 0 : 1);
        logger_->log("%:% %() % THRESH ticker:% % fair-price:%\n",
                     __FILE__, __LINE__, __FUNCTION__,
                     Common::getCurrentTimeStr(&time_str_),
                     ticker_id, bbo->toString().c_str(), fair_price);
      }

      START_MEASURE(Trading_OrderManager_moveOrders);
      order_manager_->moveOrders(ticker_id, bid_price, ask_price, clip);
      END_MEASURE(Trading_OrderManager_moveOrders, (*logger_));
    }
  }

  auto MarketMaker::onTradeUpdate(const Exchange::MEMarketUpdate *market_update,
                                   MarketOrderBook * /*book*/) noexcept -> void {
    logger_->log("%:% %() % %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 market_update->toString().c_str());
  }

  auto MarketMaker::onOrderUpdate(const Exchange::MEClientResponse *client_response)
      noexcept -> void {
    logger_->log("%:% %() % %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 client_response->toString().c_str());

    START_MEASURE(Trading_OrderManager_onOrderUpdate);
    order_manager_->onOrderUpdate(client_response);
    END_MEASURE(Trading_OrderManager_onOrderUpdate, (*logger_));
  }

}
