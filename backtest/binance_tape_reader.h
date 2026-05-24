#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "common/time_utils.h"
#include "common/types.h"
#include "exchange/market_data/market_update.h"

namespace Backtest {

  // Synthetic event stream consumed by the backtest engine. Either a top-of-book
  // refresh or a public trade, with the original event-time nanosecond stamp so
  // the engine can drive its clock from the tape.
  struct BookTopEvent {
    Common::Nanos event_ns;
    Common::Price bid_price;
    Common::Qty   bid_qty;
    Common::Price ask_price;
    Common::Qty   ask_qty;
  };

  struct TradeEvent {
    Common::Nanos event_ns;
    Common::Price price;
    Common::Qty   qty;
    Common::Side  side;          // BUY = buyer-initiated (lifted ask)
  };

  using TapeEvent = std::variant<BookTopEvent, TradeEvent>;

  // Reads a single CSV tape and yields events in chronological order.
  //
  // Two formats are supported:
  //   - "binance"  — schema documented at data.binance.vision (bookTicker and
  //                  trades formats merged by timestamp).
  //   - "synth"    — synthetic Poisson tape; used as a fallback when real data
  //                  isn't available so the demo still produces curves.
  //
  // Prices are rounded to integer ticks via `tick_size`; qtys are rounded to
  // integer lots via `qty_step`. Both default to 1 if the CSV is already in
  // integer ticks.
  class TapeReader {
  public:
    struct Config {
      std::string path;
      std::string format = "synth";   // "binance" or "synth"
      double      tick_size = 1.0;    // price units per tick (e.g. 0.1 for BTCUSDT)
      double      qty_step  = 1.0;
      uint64_t    max_events = 0;     // 0 = unlimited
    };

    explicit TapeReader(Config cfg);
    ~TapeReader();

    // Returns the next event, or std::nullopt at EOF.
    auto next() -> std::optional<TapeEvent>;

    auto eof() const { return eof_; }

    TapeReader(const TapeReader &)            = delete;
    TapeReader &operator=(const TapeReader &) = delete;

  private:
    Config        cfg_;
    std::ifstream in_;
    bool          eof_   = false;
    uint64_t      count_ = 0;

    // Synth-only state.
    uint64_t synth_seed_ = 0xC0FFEEULL;
    int64_t  synth_mid_  = 100000;      // in ticks
    Common::Nanos synth_ns_ = 0;

    auto nextBinance() -> std::optional<TapeEvent>;
    auto nextSynth()   -> std::optional<TapeEvent>;
  };
}
