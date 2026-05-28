#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>

#include "common/macros.h"

namespace Common {
  // ---- Sizing constants -------------------------------------------------
  constexpr size_t ME_MAX_TICKERS        = 8;
  constexpr size_t ME_MAX_CLIENT_UPDATES = 256 * 1024;
  constexpr size_t ME_MAX_MARKET_UPDATES = 256 * 1024;
  constexpr size_t ME_MAX_NUM_CLIENTS    = 256;
  constexpr size_t ME_MAX_ORDER_IDS      = 1024 * 1024;
  constexpr size_t ME_MAX_PRICE_LEVELS   = 256;

  // ---- Primitive named types -------------------------------------------
  typedef uint64_t OrderId;
  constexpr auto OrderId_INVALID = std::numeric_limits<OrderId>::max();

  inline auto orderIdToString(OrderId id) -> std::string {
    if (UNLIKELY(id == OrderId_INVALID)) return "INVALID";
    return std::to_string(id);
  }

  typedef uint32_t TickerId;
  constexpr auto TickerId_INVALID = std::numeric_limits<TickerId>::max();

  inline auto tickerIdToString(TickerId id) -> std::string {
    if (UNLIKELY(id == TickerId_INVALID)) return "INVALID";
    return std::to_string(id);
  }

  typedef uint32_t ClientId;
  constexpr auto ClientId_INVALID = std::numeric_limits<ClientId>::max();

  inline auto clientIdToString(ClientId id) -> std::string {
    if (UNLIKELY(id == ClientId_INVALID)) return "INVALID";
    return std::to_string(id);
  }

  typedef int64_t Price;
  constexpr auto Price_INVALID = std::numeric_limits<Price>::max();

  inline auto priceToString(Price p) -> std::string {
    if (UNLIKELY(p == Price_INVALID)) return "INVALID";
    return std::to_string(p);
  }

  typedef uint32_t Qty;
  constexpr auto Qty_INVALID = std::numeric_limits<Qty>::max();

  inline auto qtyToString(Qty q) -> std::string {
    if (UNLIKELY(q == Qty_INVALID)) return "INVALID";
    return std::to_string(q);
  }

  typedef uint64_t Priority;
  constexpr auto Priority_INVALID = std::numeric_limits<Priority>::max();

  inline auto priorityToString(Priority p) -> std::string {
    if (UNLIKELY(p == Priority_INVALID)) return "INVALID";
    return std::to_string(p);
  }

  enum class Side : int8_t {
    INVALID = 0,
    BUY     = 1,
    SELL    = -1,
    MAX     = 2
  };

  inline auto sideToString(Side side) -> std::string {
    switch (side) {
      case Side::BUY:     return "BUY";
      case Side::SELL:    return "SELL";
      case Side::INVALID: return "INVALID";
      case Side::MAX:     return "MAX";
    }
    return "UNKNOWN";
  }

  inline constexpr auto sideToIndex(Side side) noexcept {
    return static_cast<size_t>(static_cast<int>(side) + 1);
  }

  inline constexpr auto sideToValue(Side side) noexcept {
    return static_cast<int>(side);
  }

  struct RiskCfg {
    Qty    max_order_size_ = 0;
    Qty    max_position_   = 0;
    double max_loss_       = 0;

    auto toString() const {
      std::stringstream ss;
      ss << "RiskCfg{"
         << "max-order-size:" << qtyToString(max_order_size_) << " "
         << "max-position:"   << qtyToString(max_position_)   << " "
         << "max-loss:"       << max_loss_
         << "}";
      return ss.str();
    }
  };

  struct TradeEngineCfg {
    Qty    clip_      = 0;
    double threshold_ = 0;
    RiskCfg risk_cfg_;

    // v1.1 — Avellaneda-Stoikov + OFI + queue hysteresis + adaptive clip.
    // Defaults preserve the v1.0 threshold-pennying behaviour (use_as_=false).
    bool   use_as_           = false;
    double gamma_            = 0.1;    // risk aversion (AS)
    double kappa_            = 1.5;    // order arrival intensity (AS)
    double session_hours_    = 6.5;    // trading session length, for τ
    bool   use_ofi_          = false;
    double beta_ofi_         = 0.0;    // OFI loading into reservation price
    Price  hysteresis_ticks_ = 0;      // OrderManager requote dead-zone
    bool   use_adaptive_clip_ = false;
    double sigma_ref_        = 1.0;    // reference σ used in adaptive_clip scale

    // v1.2 — smoothing constants previously hardcoded in FeatureEngine.
    // Lifting them into the cfg unblocks per-strategy A/B testing and the
    // dual-EWMA regime-γ feature (uses ewma_decay_long_).
    double ewma_decay_       = 0.94;   // short-horizon σ EWMA decay
    double ofi_decay_        = 0.9;    // OFI EWMA decay
    double ewma_decay_long_  = 0.985;  // long-horizon σ EWMA decay (regime γ)
    uint64_t vol_bootstrap_  = 50;     // σ bootstrap sample count

    // Step 2 — maker rebate booked inside PositionInfo::addFill so per-fill
    // P&L reflects venue economics live (was post-hoc in analyze_pnl.py).
    // Positive = rebate credited to real_pnl; negative = fee deducted.
    // Default 0 preserves old behavior; analyze_pnl.py is told via the
    // MAKER_REBATE_BOOKED env var to not double-charge.
    double maker_rebate_bps_ = 0.0;

    // Step 3 — asymmetric OFI spread *widening* (not just reservation shift).
    // After AS spread is computed, the toxic side widens by k · |OFI|+, with
    // the other side untouched. Glosten-Milgrom 1985; Barzykin/Bergault/
    // Guéant 2025 (arxiv:2508.20225). Units: price per OFI-unit. Default 0
    // preserves prior symmetric-spread behavior.
    double spread_widen_ofi_ = 0.0;

    // Step 4 — toxic-flow killswitch. When |OFI| > killswitch_ofi_ OR
    // |microprice − mid| > killswitch_micro_ticks_ × tick, the strategy
    // cancels both sides and does NOT re-quote on this tick. Cartea/
    // Jaimungal/Penalva 2015 ch.10.4.2. Default off.
    bool   use_killswitch_       = false;
    double killswitch_ofi_       = 0.0;   // OFI magnitude triggering pull
    double killswitch_micro_ticks_ = 0.0; // microprice-mid gap in price units

    // Step 5 — volatility-regime adaptive γ. Effective γ is scaled by
    // clamp(σ_short / σ_long, 1/scale, scale): widens spread in vol spikes,
    // tightens during calm. Long-horizon σ uses ewma_decay_long_. Default
    // scale=1.0 (clamp degenerate → identical to base γ).
    bool   use_regime_gamma_     = false;
    double regime_gamma_scale_   = 1.0;

    // Step 6 — Stoikov 2018 state-dependent microprice anchor. The default
    // microprice (VWAP-bid+ask) is a lagging average; Stoikov's microprice
    // is the conditional expected mid given (spread, imbalance) which is a
    // martingale and a better short-horizon fair-value than VWAP-mid. We
    // approximate the full lookup table with a simple analytic form:
    //     micro_stoikov = mid + 0.5 · spread · (q_bid - q_ask)/(q_bid + q_ask)
    // which matches Stoikov's reported empirical relation within tick
    // granularity for crypto spot. SSRN:2970694.
    bool   use_stoikov_micro_    = false;

    // Step 7 — VPIN toxicity. Easley/López de Prado/O'Hara 2012. Trade
    // volume is bucketed; |buy − sell|/V per bucket; average over the last
    // kWindow buckets. When VPIN > vpin_threshold_ we treat the regime as
    // toxic: spread widening is scaled up and killswitch threshold is
    // scaled down. Disabled by default.
    bool   use_vpin_         = false;
    double vpin_bucket_size_ = 0.0;   // base trade-qty units per bucket
    double vpin_threshold_   = 0.46;  // crypto baseline ~0.45
    double vpin_widen_mult_  = 2.0;   // ×spread_widen_ofi_ when above thresh
    double vpin_kill_scale_  = 0.5;   // ×killswitch_ofi_  when above thresh

    auto toString() const {
      std::stringstream ss;
      ss << "TradeEngineCfg{"
         << "clip:"   << qtyToString(clip_) << " "
         << "thresh:" << threshold_         << " "
         << "as:"     << (use_as_  ? "1" : "0") << " "
         << "γ:"      << gamma_   << " "
         << "κ:"      << kappa_   << " "
         << "T:"      << session_hours_ << " "
         << "ofi:"    << (use_ofi_ ? "1" : "0") << " "
         << "β:"      << beta_ofi_ << " "
         << "hyst:"   << hysteresis_ticks_ << " "
         << "aclip:"  << (use_adaptive_clip_ ? "1" : "0") << " "
         << "σ_ref:"  << sigma_ref_ << " "
         << "rebate_bps:" << maker_rebate_bps_ << " "
         << "widen_ofi:" << spread_widen_ofi_ << " "
         << "kill:"    << (use_killswitch_ ? "1" : "0") << " "
         << "kill_ofi:"   << killswitch_ofi_ << " "
         << "kill_micro:" << killswitch_micro_ticks_ << " "
         << "regime_γ:" << (use_regime_gamma_ ? "1" : "0") << " "
         << "γ_scale:"  << regime_gamma_scale_ << " "
         << "stoikov:"  << (use_stoikov_micro_ ? "1" : "0") << " "
         << "vpin:"     << (use_vpin_ ? "1" : "0") << " "
         << "vpin_buck:" << vpin_bucket_size_ << " "
         << "vpin_thr:"  << vpin_threshold_ << " "
         << "risk:"   << risk_cfg_.toString()
         << "}";
      return ss.str();
    }
  };

  typedef std::array<TradeEngineCfg, ME_MAX_TICKERS> TradeEngineCfgHashMap;

  // ---- Chapter 10: trading strategy selection -------------------------
  enum class AlgoType : int8_t {
    INVALID = 0,
    RANDOM  = 1,
    MAKER   = 2,
    TAKER   = 3,
    MAX     = 4
  };

  inline auto algoTypeToString(AlgoType type) -> std::string {
    switch (type) {
      case AlgoType::RANDOM:  return "RANDOM";
      case AlgoType::MAKER:   return "MAKER";
      case AlgoType::TAKER:   return "TAKER";
      case AlgoType::INVALID: return "INVALID";
      case AlgoType::MAX:     return "MAX";
    }
    return "UNKNOWN";
  }

  inline auto stringToAlgoType(const std::string &str) -> AlgoType {
    for (auto i = static_cast<int>(AlgoType::INVALID);
         i <= static_cast<int>(AlgoType::MAX); ++i) {
      const auto algo_type = static_cast<AlgoType>(i);
      if (algoTypeToString(algo_type) == str)
        return algo_type;
    }
    return AlgoType::INVALID;
  }
}
