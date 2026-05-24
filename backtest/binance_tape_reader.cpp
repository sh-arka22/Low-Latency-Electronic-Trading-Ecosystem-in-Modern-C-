#include "backtest/binance_tape_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace Backtest {

  TapeReader::TapeReader(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.format == "binance") {
      in_.open(cfg_.path);
      if (!in_) throw std::runtime_error("TapeReader: cannot open " + cfg_.path);
    }
  }

  TapeReader::~TapeReader() {
    if (in_.is_open()) in_.close();
  }

  auto TapeReader::next() -> std::optional<TapeEvent> {
    if (eof_) return std::nullopt;
    if (cfg_.max_events && count_ >= cfg_.max_events) {
      eof_ = true;
      return std::nullopt;
    }
    auto ev = (cfg_.format == "binance") ? nextBinance() : nextSynth();
    if (ev) ++count_;
    return ev;
  }

  // ----------------------------------------------------------------------
  // Binance CSV (pre-merged book+trade by event_time).
  //
  // The expected schema is the union of bookTicker and trades, with a
  // discriminator column. Pre-process with the bundled merger script:
  //   ts_ns,kind,price,qty,bid_price,bid_qty,ask_price,ask_qty,is_buyer_maker
  // where kind = "B" for book updates (uses bid/ask cols) or "T" for trades
  // (uses price/qty/is_buyer_maker cols).
  // ----------------------------------------------------------------------
  auto TapeReader::nextBinance() -> std::optional<TapeEvent> {
    std::string line;
    while (std::getline(in_, line)) {
      if (line.empty()) continue;
      if (line[0] == '#' || line[0] == 't') continue;   // skip header

      std::stringstream ss(line);
      std::string ts_str, kind, p_str, q_str, bp_str, bq_str, ap_str, aq_str, ibm_str;
      if (!std::getline(ss, ts_str, ','))    continue;
      if (!std::getline(ss, kind, ','))      continue;
      if (!std::getline(ss, p_str, ','))     continue;
      if (!std::getline(ss, q_str, ','))     continue;
      if (!std::getline(ss, bp_str, ','))    continue;
      if (!std::getline(ss, bq_str, ','))    continue;
      if (!std::getline(ss, ap_str, ','))    continue;
      if (!std::getline(ss, aq_str, ','))    continue;
      std::getline(ss, ibm_str, ',');

      const auto ts_ns = static_cast<Common::Nanos>(std::stoll(ts_str));
      const auto to_ticks = [&](const std::string &s) -> Common::Price {
        if (s.empty()) return Common::Price_INVALID;
        return static_cast<Common::Price>(std::llround(std::stod(s) / cfg_.tick_size));
      };
      const auto to_lots = [&](const std::string &s) -> Common::Qty {
        if (s.empty()) return Common::Qty_INVALID;
        return static_cast<Common::Qty>(std::max(1.0,
            std::round(std::stod(s) / cfg_.qty_step)));
      };

      if (kind == "B") {
        const BookTopEvent b{ts_ns,
                              to_ticks(bp_str), to_lots(bq_str),
                              to_ticks(ap_str), to_lots(aq_str)};
        return TapeEvent{b};
      }
      if (kind == "T") {
        // is_buyer_maker == true means seller initiated (price went down).
        const bool buyer_maker = (!ibm_str.empty() && (ibm_str[0]=='t' || ibm_str[0]=='1'));
        const Common::Side side = buyer_maker ? Common::Side::SELL : Common::Side::BUY;
        const TradeEvent t{ts_ns, to_ticks(p_str), to_lots(q_str), side};
        return TapeEvent{t};
      }
    }
    eof_ = true;
    return std::nullopt;
  }

  // ----------------------------------------------------------------------
  // Synthetic Poisson tape — random-walk mid + Poisson-arrival trades, used
  // when real data isn't available. Deterministic given the seed so the
  // four-strategy comparison is reproducible.
  // ----------------------------------------------------------------------
  auto TapeReader::nextSynth() -> std::optional<TapeEvent> {
    if (count_ >= 200'000) { eof_ = true; return std::nullopt; }

    // xorshift64*
    auto rng = [&]() {
      uint64_t x = synth_seed_;
      x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
      synth_seed_ = x;
      return x * 2685821657736338717ULL;
    };
    auto uniform = [&](double lo, double hi) {
      return lo + (hi - lo) * (static_cast<double>(rng() >> 11)
                              / static_cast<double>(1ULL << 53));
    };

    // ~50 µs between events, with a fat right tail (Pareto-ish via inverse exp).
    const double u = std::max(1e-9, uniform(0.0, 1.0));
    const auto dt_ns = static_cast<Common::Nanos>(-std::log(u) * 50'000.0);
    synth_ns_ += dt_ns;

    // Mean-reverting walk: pull mid toward the anchor each step so all touched
    // prices stay inside a ±100-tick band. ME_MAX_PRICE_LEVELS=256 means
    // priceToIndex collides outside that window, so bounding is mandatory.
    static constexpr int64_t kAnchor   = 100000;
    static constexpr int64_t kBandHalf = 100;
    if (uniform(0.0, 1.0) < 0.55) {
      synth_mid_ += (uniform(0.0, 1.0) < 0.5) ? +1 : -1;
    }
    if (uniform(0.0, 1.0) < 0.002) {
      synth_mid_ += (uniform(0.0, 1.0) < 0.5) ? +5 : -5;
    }
    // Soft pull back toward the anchor.
    synth_mid_ -= (synth_mid_ - kAnchor) / 64;
    // Hard clamp.
    if (synth_mid_ < kAnchor - kBandHalf) synth_mid_ = kAnchor - kBandHalf;
    if (synth_mid_ > kAnchor + kBandHalf) synth_mid_ = kAnchor + kBandHalf;

    // Trade vs book-update probability.
    // Bias the event mix toward trades so fills happen in a reasonable window.
    // Top-of-book depth is kept thin (1-3 lots) so a 5-lot resting algo order
    // gets to the front of the queue within a couple of trade events — without
    // this the backtest just shows zero fills across all strategies and the
    // comparison loses signal.
    if (uniform(0.0, 1.0) < 0.55) {
      const auto side = uniform(0.0, 1.0) < 0.5 ? Common::Side::BUY : Common::Side::SELL;
      const auto px   = static_cast<Common::Price>(
          synth_mid_ + (side == Common::Side::BUY ? +1 : -1));
      const auto qty  = static_cast<Common::Qty>(1 + (rng() % 4));
      const TradeEvent t{synth_ns_, px, qty, side};
      return TapeEvent{t};
    } else {
      const auto bid_qty = static_cast<Common::Qty>(1 + (rng() % 3));
      const auto ask_qty = static_cast<Common::Qty>(1 + (rng() % 3));
      const BookTopEvent b{synth_ns_,
                            static_cast<Common::Price>(synth_mid_ - 1), bid_qty,
                            static_cast<Common::Price>(synth_mid_ + 1), ask_qty};
      return TapeEvent{b};
    }
  }
}
