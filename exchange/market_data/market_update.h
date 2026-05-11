#pragma once

#include <sstream>

#include "common/lf_queue.h"
#include "common/types.h"

using namespace Common;

namespace Exchange {
#pragma pack(push, 1)

  /// Ch7: CLEAR / SNAPSHOT_START / SNAPSHOT_END added for snapshot recovery.
  /// Numeric values changed from Ch6 because new values are inserted.
  enum class MarketUpdateType : uint8_t {
    INVALID        = 0,
    CLEAR          = 1,   // tells recipient to wipe its book for the ticker
    ADD            = 2,
    MODIFY         = 3,
    CANCEL         = 4,
    TRADE          = 5,
    SNAPSHOT_START = 6,   // start of a snapshot cycle
    SNAPSHOT_END   = 7    // end of cycle; order_id_ carries last incremental seq
  };

  inline std::string marketUpdateTypeToString(MarketUpdateType type) {
    switch (type) {
      case MarketUpdateType::CLEAR:          return "CLEAR";
      case MarketUpdateType::ADD:            return "ADD";
      case MarketUpdateType::MODIFY:         return "MODIFY";
      case MarketUpdateType::CANCEL:         return "CANCEL";
      case MarketUpdateType::TRADE:          return "TRADE";
      case MarketUpdateType::SNAPSHOT_START: return "SNAPSHOT_START";
      case MarketUpdateType::SNAPSHOT_END:   return "SNAPSHOT_END";
      case MarketUpdateType::INVALID:        return "INVALID";
    }
    return "UNKNOWN";
  }

  /// Internal market update — Matching Engine -> MarketDataPublisher (in-process).
  struct MEMarketUpdate {
    MarketUpdateType type_      = MarketUpdateType::INVALID;
    OrderId          order_id_  = OrderId_INVALID;
    TickerId         ticker_id_ = TickerId_INVALID;
    Side             side_      = Side::INVALID;
    Price            price_     = Price_INVALID;
    Qty              qty_       = Qty_INVALID;
    Priority         priority_  = Priority_INVALID;

    auto toString() const {
      std::stringstream ss;
      ss << "MEMarketUpdate ["
         << " type:"     << marketUpdateTypeToString(type_)
         << " ticker:"   << tickerIdToString(ticker_id_)
         << " oid:"      << orderIdToString(order_id_)
         << " side:"     << sideToString(side_)
         << " qty:"      << qtyToString(qty_)
         << " price:"    << priceToString(price_)
         << " priority:" << priorityToString(priority_)
         << "]";
      return ss.str();
    }
  };

  /// Wire format for UDP multicast: per-packet sequence number + payload.
  /// Stays inside the same pack(1) block as MEMarketUpdate.
  struct MDPMarketUpdate {
    size_t         seq_num_ = 0;
    MEMarketUpdate me_market_update_;

    auto toString() const {
      std::stringstream ss;
      ss << "MDPMarketUpdate ["
         << " seq:" << seq_num_
         << " "     << me_market_update_.toString()
         << "]";
      return ss.str();
    }
  };

#pragma pack(pop)

  typedef Common::LFQueue<Exchange::MEMarketUpdate>  MEMarketUpdateLFQueue;
  typedef Common::LFQueue<Exchange::MDPMarketUpdate> MDPMarketUpdateLFQueue;  // Ch7
}
