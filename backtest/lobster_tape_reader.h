#pragma once

#include <cstdint>
#include <deque>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/time_utils.h"
#include "common/types.h"
#include "exchange/market_data/market_update.h"

namespace Backtest {

  // Replays a LOBSTER (NASDAQ) sample as the engine's NATIVE market-update
  // stream, driven by the **orderbook** file -- the approach recommended by
  // LOBSTER and by practitioners for a *windowed* sample.
  //
  // Rationale: LOBSTER's docs state that message row k is the single event
  // taking the orderbook from row k-1 to row k, and the orderbook file is
  // LOBSTER's own authoritative full-depth reconstruction. A *windowed* message
  // file omits the ADDs of orders resting before the window start, so naive
  // message-replay into an empty book leaks stale orders and the book CROSSES
  // (verified). Instead we DIFF consecutive orderbook snapshots into native
  // Exchange::MEMarketUpdate ADD/MODIFY/CANCEL per price level (one aggregate
  // order per occupied level; deterministic OID = side-offset + price), seeded
  // from the opening snapshot. This reproduces LOBSTER's exact, non-crossing
  // full-depth book inside the engine's real MarketOrderBook. The message file
  // supplies the event timestamp and flags executions (types 4/5) as TRADE
  // prints (the only events that should drive fills / order-flow features).
  //
  // Honest scope: this is faithful market-by-price (exact depth, never crosses).
  // True per-order queue identity is NOT recoverable from a windowed sample --
  // that needs full-session ITCH from system open (Moallemi 2016; ITCH recon
  // threads). Queue position for our own orders is therefore approximated from
  // real book depth + trade-burn (the standard maker fill model).
  class LobsterReader {
  public:
    struct Config {
      std::string      message_path;   // *_message_*.csv (orderbook path derived)
      Common::TickerId ticker_id  = 0;
      int              levels      = 10;
      uint64_t         max_events  = 0; // 0 = unlimited (counts steps/rows)
    };

    explicit LobsterReader(Config cfg);
    ~LobsterReader();

    // Next (event_ns, update). One orderbook step expands into a TRADE print
    // (if the step was an execution) followed by the ADD/MODIFY/CANCEL diffs.
    auto next() -> std::optional<std::pair<Common::Nanos, Exchange::MEMarketUpdate>>;

    LobsterReader(const LobsterReader &)            = delete;
    LobsterReader &operator=(const LobsterReader &) = delete;

  private:
    Config           cfg_;
    std::ifstream    msg_in_;
    std::ifstream    ob_in_;
    bool             eof_   = false;
    uint64_t         count_ = 0;
    Common::Priority next_priority_ = 1;

    // Previous snapshot's occupied levels (price-cents -> aggregate qty).
    std::unordered_map<Common::Price, Common::Qty> prev_bid_;
    std::unordered_map<Common::Price, Common::Qty> prev_ask_;

    // Updates produced for the current step, drained by next().
    std::deque<std::pair<Common::Nanos, Exchange::MEMarketUpdate>> pending_;

    // Reads one (message,orderbook) row pair and pushes its updates to pending_.
    // Returns false at EOF.
    auto fillStep() -> bool;
  };
}
