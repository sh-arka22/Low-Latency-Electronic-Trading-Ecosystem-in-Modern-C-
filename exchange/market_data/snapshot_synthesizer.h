#pragma once

#include <array>
#include <string>

#include "common/lf_queue.h"
#include "common/logging.h"
#include "common/macros.h"
#include "common/mcast_socket.h"
#include "common/mem_pool.h"
#include "common/time_utils.h"
#include "common/types.h"
#include "market_data/market_update.h"

using namespace Common;

namespace Exchange {
  /// Runs on its own thread. Maintains an in-memory snapshot of all live orders
  /// (per ticker), then periodically blasts the whole snapshot over UDP
  /// multicast on the snapshot stream so late joiners can recover.
  class SnapshotSynthesizer final {
  public:
    SnapshotSynthesizer(MDPMarketUpdateLFQueue *market_updates,
                        const std::string &iface,
                        const std::string &snapshot_ip,
                        int snapshot_port);

    ~SnapshotSynthesizer();

    auto start() -> void;
    auto stop()  -> void;

    /// Apply one incremental update to our in-memory book.
    auto addToSnapshot(const MDPMarketUpdate *market_update) -> void;

    /// Walk every ticker, send SNAPSHOT_START + CLEAR/ADD frames + SNAPSHOT_END.
    auto publishSnapshot() -> void;

    auto run() -> void;

    SnapshotSynthesizer() = delete;
    SnapshotSynthesizer(const SnapshotSynthesizer &) = delete;
    SnapshotSynthesizer(const SnapshotSynthesizer &&) = delete;
    SnapshotSynthesizer &operator=(const SnapshotSynthesizer &) = delete;
    SnapshotSynthesizer &operator=(const SnapshotSynthesizer &&) = delete;

  private:
    MDPMarketUpdateLFQueue *snapshot_md_updates_ = nullptr;  // FROM publisher

    Logger        logger_;
    volatile bool run_ = false;
    std::string   time_str_;

    McastSocket snapshot_socket_;

    /// ticker_orders_[ticker][order_id] = MEMarketUpdate* (nullptr = no order).
    std::array<std::array<MEMarketUpdate *, ME_MAX_ORDER_IDS>, ME_MAX_TICKERS>
        ticker_orders_;

    size_t last_inc_seq_num_  = 0;
    Nanos  last_snapshot_time_ = 0;

    MemPool<MEMarketUpdate> order_pool_;
  };
}
