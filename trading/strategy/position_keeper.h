#pragma once

#include <array>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

#include "common/macros.h"
#include "common/types.h"
#include "common/logging.h"
#include "exchange/order_server/client_response.h"
#include "strategy/market_order.h"

using namespace Common;

namespace Trading {

  struct PositionInfo {
    int32_t position_   = 0;
    double  real_pnl_   = 0,
            unreal_pnl_ = 0,
            total_pnl_  = 0;
    std::array<double, sideToIndex(Side::MAX) + 1> open_vwap_;
    Qty        volume_ = 0;
    const BBO *bbo_    = nullptr;

    auto toString() const {
      std::stringstream ss;
      ss << "Position{"
         << "pos:"        << position_   << " "
         << "real-pnl:"   << real_pnl_   << " "
         << "unreal-pnl:" << unreal_pnl_ << " "
         << "total-pnl:"  << total_pnl_  << " "
         << "vol:"        << qtyToString(volume_) << " "
         << "vwaps[" << (position_ ? open_vwap_[sideToIndex(Side::BUY)]  / std::abs(position_) : 0) << "X"
                    << (position_ ? open_vwap_[sideToIndex(Side::SELL)] / std::abs(position_) : 0) << "] "
         << (bbo_ ? bbo_->toString() : "")
         << "}";
      return ss.str();
    }

    auto addFill(const Exchange::MEClientResponse *client_response, Logger *logger,
                 double maker_rebate_bps = 0.0) noexcept {
      const auto old_position   = position_;
      const auto side_index     = sideToIndex(client_response->side_);
      const auto opp_side_index = sideToIndex(
          client_response->side_ == Side::BUY ? Side::SELL : Side::BUY);
      const auto side_value     = sideToValue(client_response->side_);

      position_ += static_cast<int32_t>(client_response->exec_qty_) * side_value;
      volume_   += client_response->exec_qty_;

      if (old_position * sideToValue(client_response->side_) >= 0) {
        open_vwap_[side_index] += (client_response->price_ * client_response->exec_qty_);
      } else {
        const auto opp_side_vwap = open_vwap_[opp_side_index] / std::abs(old_position);
        open_vwap_[opp_side_index] = opp_side_vwap * std::abs(position_);

        real_pnl_ += std::min(static_cast<int32_t>(client_response->exec_qty_),
                              std::abs(old_position))
                     * (opp_side_vwap - client_response->price_)
                     * sideToValue(client_response->side_);

        if (position_ * old_position < 0) {
          open_vwap_[side_index]     = client_response->price_ * std::abs(position_);
          open_vwap_[opp_side_index] = 0;
        }
      }

      if (!position_) {
        open_vwap_[sideToIndex(Side::BUY)]  = 0;
        open_vwap_[sideToIndex(Side::SELL)] = 0;
        unreal_pnl_ = 0;
      } else {
        if (position_ > 0)
          unreal_pnl_ = (client_response->price_
                         - open_vwap_[sideToIndex(Side::BUY)] / std::abs(position_))
                        * std::abs(position_);
        else
          unreal_pnl_ = (open_vwap_[sideToIndex(Side::SELL)] / std::abs(position_)
                         - client_response->price_)
                        * std::abs(position_);
      }

      total_pnl_ = unreal_pnl_ + real_pnl_;

      // Step 2 — book maker rebate (or fee) on every fill so per-fill P&L
      // reflects venue economics live. Positive bps = rebate credited;
      // negative = fee deducted. Default 0 preserves prior post-hoc fee
      // accounting in analyze_pnl.py.
      if (maker_rebate_bps != 0.0) {
        const double notional = static_cast<double>(client_response->price_)
                              * static_cast<double>(client_response->exec_qty_);
        const double rebate   = notional * maker_rebate_bps * 1e-4;
        real_pnl_  += rebate;
        total_pnl_ += rebate;
      }

      std::string time_str;
      logger->log("%:% %() % % %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str),
                  toString(), client_response->toString().c_str());
    }

    auto updateBBO(const BBO *bbo, Logger *logger) noexcept {
      std::string time_str;
      bbo_ = bbo;

      if (position_ && bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID) {
        const auto mid_price = (bbo->bid_price_ + bbo->ask_price_) * 0.5;

        if (position_ > 0)
          unreal_pnl_ = (mid_price
                         - open_vwap_[sideToIndex(Side::BUY)] / std::abs(position_))
                        * std::abs(position_);
        else
          unreal_pnl_ = (open_vwap_[sideToIndex(Side::SELL)] / std::abs(position_)
                         - mid_price)
                        * std::abs(position_);

        const auto old_total_pnl = total_pnl_;
        total_pnl_ = unreal_pnl_ + real_pnl_;

        if (total_pnl_ != old_total_pnl)
          logger->log("%:% %() % % %\n", __FILE__, __LINE__, __FUNCTION__,
                      Common::getCurrentTimeStr(&time_str),
                      toString(), bbo_->toString());
      }
    }
  };

  class PositionKeeper {
  public:
    // Step 2 — store per-ticker rebate bps BY VALUE at construction so we
    // don't depend on the caller's TradeEngineCfgHashMap outliving us.
    // (Earlier ptr-to-caller-local was dangling once BacktestEngine's ctor
    // returned, silently disabling the rebate.)
    PositionKeeper(Common::Logger *logger,
                   const TradeEngineCfgHashMap *ticker_cfg = nullptr)
        : logger_(logger) {
      for (size_t i = 0; i < rebate_bps_.size(); ++i)
        rebate_bps_[i] = ticker_cfg ? ticker_cfg->at(i).maker_rebate_bps_ : 0.0;
    }

    auto addFill(const Exchange::MEClientResponse *client_response) noexcept {
      const double rebate_bps = rebate_bps_.at(client_response->ticker_id_);
      ticker_position_.at(client_response->ticker_id_)
                      .addFill(client_response, logger_, rebate_bps);
    }

    auto updateBBO(TickerId ticker_id, const BBO *bbo) noexcept {
      ticker_position_.at(ticker_id).updateBBO(bbo, logger_);
    }

    auto getPositionInfo(TickerId ticker_id) const noexcept {
      return &(ticker_position_.at(ticker_id));
    }

    auto toString() const {
      double total_pnl = 0;
      Qty    total_vol = 0;
      std::stringstream ss;
      for (TickerId i = 0; i < ticker_position_.size(); ++i) {
        ss << "TickerId:" << tickerIdToString(i) << " "
           << ticker_position_.at(i).toString() << "\n";
        total_pnl += ticker_position_.at(i).total_pnl_;
        total_vol += ticker_position_.at(i).volume_;
      }
      ss << "Total PnL:" << total_pnl << " Vol:" << total_vol << "\n";
      return ss.str();
    }

    PositionKeeper()                                   = delete;
    PositionKeeper(const PositionKeeper &)             = delete;
    PositionKeeper(const PositionKeeper &&)            = delete;
    PositionKeeper &operator=(const PositionKeeper &)  = delete;
    PositionKeeper &operator=(const PositionKeeper &&) = delete;

  private:
    std::string  time_str_;
    Common::Logger *logger_ = nullptr;
    std::array<double, ME_MAX_TICKERS> rebate_bps_{};   // per-ticker, by value
    std::array<PositionInfo, ME_MAX_TICKERS> ticker_position_;
  };
}
