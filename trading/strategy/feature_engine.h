#pragma once

#include <cmath>
#include <limits>

#include "common/macros.h"
#include "common/logging.h"
#include "common/types.h"
#include "market_data/market_update.h"
#include "strategy/market_order_book.h"

using namespace Common;

namespace Trading {

  constexpr auto Feature_INVALID = std::numeric_limits<double>::quiet_NaN();

  class FeatureEngine {
  public:
    FeatureEngine(Common::Logger *logger)
        : logger_(logger) {}

    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side,
                           MarketOrderBook *book) noexcept -> void {
      const auto bbo = book->getBBO();
      if (LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID)) {
        // ----- mkt_price_ (micro-price), unchanged from v1.0 ---------------
        mkt_price_ = (bbo->bid_price_ * bbo->ask_qty_ + bbo->ask_price_ * bbo->bid_qty_)
                     / static_cast<double>(bbo->bid_qty_ + bbo->ask_qty_);

        // ----- EWMA realised volatility on mid-price returns ---------------
        const auto mid = 0.5 * (bbo->bid_price_ + bbo->ask_price_);
        if (prev_mid_ != Feature_INVALID) {
          const auto ret = mid - prev_mid_;
          ewma_variance_ = kEwmaDecay * ewma_variance_
                         + (1.0 - kEwmaDecay) * ret * ret;
          if (++vol_samples_ >= kVolBootstrap)
            volatility_ = std::sqrt(ewma_variance_);
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
          ofi_ = kOfiDecay * ofi_ + (1.0 - kOfiDecay) * e_n;
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
      logger_->log("%:% %() % % mkt-price:% σ:% ofi:% agg-trade-ratio:%\n",
                   __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(&time_str_),
                   market_update->toString().c_str(),
                   mkt_price_, volatility_, ofi_, agg_trade_qty_ratio_);
    }

    auto getMktPrice()         const noexcept { return mkt_price_; }
    auto getAggTradeQtyRatio() const noexcept { return agg_trade_qty_ratio_; }
    auto getVolatility()       const noexcept { return volatility_; }
    auto getOFI()              const noexcept { return ofi_; }

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

    // ---- EWMA volatility -----------------------------------------------
    double prev_mid_       = Feature_INVALID;
    double ewma_variance_  = 0.0;
    double volatility_     = Feature_INVALID;
    uint64_t vol_samples_  = 0;
    static constexpr double kEwmaDecay   = 0.94;   // ~60-sample half-life
    static constexpr uint64_t kVolBootstrap = 50;  // skip σ until warmed up

    // ---- OFI (CKS 2014) ------------------------------------------------
    Price prev_bid_price_ = Price_INVALID;
    Qty   prev_bid_qty_   = Qty_INVALID;
    Price prev_ask_price_ = Price_INVALID;
    Qty   prev_ask_qty_   = Qty_INVALID;
    double ofi_           = 0.0;
    static constexpr double kOfiDecay = 0.9;
  };
}
