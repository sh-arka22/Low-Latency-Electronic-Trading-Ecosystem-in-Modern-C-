#pragma once

#include <cmath>
#include <limits>

#include "common/macros.h"
#include "common/logging.h"
#include "common/types.h"
#include "market_data/market_update.h"
#include "strategy/market_order_book.h"
#include "strategy/vpin.h"

using namespace Common;

namespace Trading {

  constexpr auto Feature_INVALID = std::numeric_limits<double>::quiet_NaN();

  class FeatureEngine {
  public:
    // v1.2 — cfg ref lifts the EWMA decay / bootstrap constants out of
    // constexpr so per-strategy sweeps and Step 5's regime-γ (which needs a
    // second long-horizon decay) become tunable. Defaults preserve v1.1
    // behavior exactly: ewma_decay_=0.94, vol_bootstrap_=50, ofi_decay_=0.9.
    FeatureEngine(Common::Logger *logger,
                  const Common::TradeEngineCfg &cfg)
        : logger_(logger),
          ewma_decay_(cfg.ewma_decay_),
          ewma_decay_long_(cfg.ewma_decay_long_),
          vol_bootstrap_(cfg.vol_bootstrap_),
          use_stoikov_micro_(cfg.use_stoikov_micro_),
          ofi_decay_(cfg.ofi_decay_),
          use_vpin_(cfg.use_vpin_) {
      if (cfg.use_vpin_ && cfg.vpin_bucket_size_ > 0.0)
        vpin_.setBucketSize(cfg.vpin_bucket_size_);
    }

    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side,
                           MarketOrderBook *book) noexcept -> void {
      const auto bbo = book->getBBO();
      if (LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID)) {
        // ----- mkt_price_ (micro-price) ---------------------------------
        // Default = VWAP-of-touch (v1.0). Step 6 switches in Stoikov's
        // state-dependent microprice: mid + 0.5·spread·imbalance. Approxi-
        // mation of Stoikov 2018's discrete-state lookup that captures the
        // dominant effect (imbalance-weighted half-spread shift) without
        // requiring an offline-precomputed transition table.
        const double total_q = static_cast<double>(bbo->bid_qty_ + bbo->ask_qty_);
        if (use_stoikov_micro_) {
          const double mid_now = 0.5 * (bbo->bid_price_ + bbo->ask_price_);
          const double spread  = static_cast<double>(bbo->ask_price_ - bbo->bid_price_);
          const double imb     = total_q > 0
              ? (static_cast<double>(bbo->bid_qty_) - static_cast<double>(bbo->ask_qty_)) / total_q
              : 0.0;
          mkt_price_ = mid_now + 0.5 * spread * imb;
        } else {
          mkt_price_ = (bbo->bid_price_ * bbo->ask_qty_ + bbo->ask_price_ * bbo->bid_qty_)
                       / total_q;
        }

        // ----- EWMA realised volatility on mid-price returns ---------------
        // NB: prev_mid_'s sentinel is NaN, and `NaN != NaN` is true in IEEE
        // 754 — direct inequality with Feature_INVALID would *enter* the
        // branch on the first event, compute ret = mid - NaN = NaN, and
        // poison ewma_variance_ permanently. Use isnan() instead.
        const auto mid = 0.5 * (bbo->bid_price_ + bbo->ask_price_);
        if (!std::isnan(prev_mid_)) {
          const auto ret = mid - prev_mid_;
          ewma_variance_ = ewma_decay_ * ewma_variance_
                         + (1.0 - ewma_decay_) * ret * ret;
          // Step 5 — long-horizon EWMA tracks regime, σ_short/σ_long ratio
          // drives gamma_eff in market_maker.cpp. Always-on plumbing; mat-
          // erialized only when use_regime_gamma_ is set.
          ewma_variance_long_ = ewma_decay_long_ * ewma_variance_long_
                              + (1.0 - ewma_decay_long_) * ret * ret;
          if (++vol_samples_ >= vol_bootstrap_) {
            volatility_      = std::sqrt(ewma_variance_);
            volatility_long_ = std::sqrt(ewma_variance_long_);
          }
        }
        prev_mid_ = mid;

        // ----- OFI (Cont, Kukanov, Stoikov 2014, eq. (1)) ------------------
        // Per-side signed contribution then EWMA-smoothed.
        if (prev_bid_price_ != Price_INVALID && prev_ask_price_ != Price_INVALID) {
          double e_bid = 0.0, e_ask = 0.0;
          if (bbo->bid_price_ > prev_bid_price_)
            e_bid = static_cast<double>(bbo->bid_qty_);
          else if (bbo->bid_price_ == prev_bid_price_)
            e_bid = static_cast<double>(bbo->bid_qty_) - static_cast<double>(prev_bid_qty_);
          else
            e_bid = -static_cast<double>(prev_bid_qty_);

          if (bbo->ask_price_ < prev_ask_price_)
            e_ask = static_cast<double>(bbo->ask_qty_);
          else if (bbo->ask_price_ == prev_ask_price_)
            e_ask = static_cast<double>(bbo->ask_qty_) - static_cast<double>(prev_ask_qty_);
          else
            e_ask = -static_cast<double>(prev_ask_qty_);

          const auto e_n = e_bid - e_ask;
          ofi_ = ofi_decay_ * ofi_ + (1.0 - ofi_decay_) * e_n;
        }
        prev_bid_price_ = bbo->bid_price_;
        prev_bid_qty_   = bbo->bid_qty_;
        prev_ask_price_ = bbo->ask_price_;
        prev_ask_qty_   = bbo->ask_qty_;
      }

      logger_->log("%:% %() % ticker:% price:% side:% mkt-price:% σ:% ofi:% agg-trade-ratio:%\n",
                   __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(&time_str_),
                   ticker_id,
                   Common::priceToString(price).c_str(),
                   Common::sideToString(side).c_str(),
                   mkt_price_, volatility_, ofi_, agg_trade_qty_ratio_);
    }

    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update,
                       MarketOrderBook *book) noexcept -> void {
      const auto bbo = book->getBBO();
      if (LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID)) {
        agg_trade_qty_ratio_ = static_cast<double>(market_update->qty_)
                               / (market_update->side_ == Side::BUY
                                  ? bbo->ask_qty_
                                  : bbo->bid_qty_);
      }

      // Step 7 — VPIN bucketing. Tape's `side_` is the aggressor; BUY-side
      // aggressor = buyer-initiated trade in BVC terms.
      if (use_vpin_) {
        vpin_.onTrade(static_cast<double>(market_update->qty_),
                      market_update->side_ == Side::BUY);
      }
      logger_->log("%:% %() % % mkt-price:% σ:% ofi:% agg-trade-ratio:%\n",
                   __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(&time_str_),
                   market_update->toString().c_str(),
                   mkt_price_, volatility_, ofi_, agg_trade_qty_ratio_);
    }

    auto getMktPrice()         const noexcept { return mkt_price_; }
    auto getAggTradeQtyRatio() const noexcept { return agg_trade_qty_ratio_; }
    auto getVolatility()       const noexcept { return volatility_; }
    auto getVolatilityLong()   const noexcept { return volatility_long_; }
    auto getOFI()              const noexcept { return ofi_; }
    auto getVPIN()             const noexcept { return vpin_.value(); }

    FeatureEngine() = delete;
    FeatureEngine(const FeatureEngine &)  = delete;
    FeatureEngine(const FeatureEngine &&) = delete;
    FeatureEngine &operator=(const FeatureEngine &)  = delete;
    FeatureEngine &operator=(const FeatureEngine &&) = delete;

  private:
    std::string    time_str_;
    Common::Logger *logger_ = nullptr;

    double mkt_price_           = Feature_INVALID;
    double agg_trade_qty_ratio_ = Feature_INVALID;

    // ---- EWMA volatility (short + long horizon) -----------------------
    double prev_mid_            = Feature_INVALID;
    double ewma_variance_       = 0.0;
    double ewma_variance_long_  = 0.0;
    double volatility_          = Feature_INVALID;
    double volatility_long_     = Feature_INVALID;
    uint64_t vol_samples_       = 0;
    double ewma_decay_;       // short EWMA decay (~60-sample halflife at 0.94)
    double ewma_decay_long_;  // long EWMA decay  (~400-sample halflife at 0.985)
    uint64_t vol_bootstrap_;  // skip σ until warmed up
    bool   use_stoikov_micro_;// Step 6 — Stoikov microprice anchor

    // ---- OFI (CKS 2014) ------------------------------------------------
    Price prev_bid_price_ = Price_INVALID;
    Qty   prev_bid_qty_   = Qty_INVALID;
    Price prev_ask_price_ = Price_INVALID;
    Qty   prev_ask_qty_   = Qty_INVALID;
    double ofi_           = 0.0;
    double ofi_decay_;

    // ---- VPIN (Step 7) -------------------------------------------------
    static constexpr std::size_t kVpinWindow = 50;
    Vpin<kVpinWindow> vpin_;
    bool   use_vpin_;
  };
}
