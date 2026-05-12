#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <thread>

#include "common/logging.h"
#include "common/macros.h"
#include "common/mcast_socket.h"
#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "common/types.h"
#include "market_data/market_update.h"

namespace Trading {
  /// UDP multicast subscriber. Owns two sockets:
  ///   - incremental: always joined, every individual book update with monotonic seq
  ///   - snapshot:    joined only when a gap is detected (init/join in startSnapshotSync,
  ///                  leave in checkSnapshotSync once recovery completes)
  /// On the happy path, incremental messages are forwarded straight to the
  /// MEMarketUpdateLFQueue. On a gap, the consumer enters recovery, buffers both
  /// streams keyed by seq_num in std::map, and only drains the merged sequence
  /// to the LFQueue once a full snapshot + matching incrementals are present.
  class MarketDataConsumer {
  public:
    MarketDataConsumer(Common::ClientId client_id,
                       Exchange::MEMarketUpdateLFQueue *market_updates,
                       const std::string &iface,
                       const std::string &snapshot_ip, int snapshot_port,
                       const std::string &incremental_ip, int incremental_port);

    ~MarketDataConsumer() {
      stop();
      using namespace std::literals::chrono_literals;
      std::this_thread::sleep_for(5s);
    }

    auto start() {
      run_ = true;
      ASSERT(Common::createAndStartThread(-1, "Trading/MarketDataConsumer",
                                          [this]() { run(); }) != nullptr,
             "Failed to start MarketData thread.");
    }

    auto stop() -> void { run_ = false; }

    MarketDataConsumer() = delete;
    MarketDataConsumer(const MarketDataConsumer &) = delete;
    MarketDataConsumer(const MarketDataConsumer &&) = delete;
    MarketDataConsumer &operator=(const MarketDataConsumer &) = delete;
    MarketDataConsumer &operator=(const MarketDataConsumer &&) = delete;

  private:
    /// Expected seq_num on the next incremental message. Resets to
    /// (SNAPSHOT_END.order_id_ + 1) when recovery completes.
    size_t next_exp_inc_seq_num_ = 1;
    Exchange::MEMarketUpdateLFQueue *incoming_md_updates_ = nullptr;

    volatile bool run_ = false;
    std::string   time_str_;
    Common::Logger logger_;

    Common::McastSocket incremental_mcast_socket_;
    Common::McastSocket snapshot_mcast_socket_;

    bool             in_recovery_   = false;
    const std::string iface_, snapshot_ip_;
    const int         snapshot_port_;

    // Recovery buffers — keyed by seq_num so iteration is in sequence order.
    // std::map is the one exception to the no-dynamic-allocation rule: recovery
    // is rare, trading is paused while it runs, and we need sorted iteration.
    typedef std::map<size_t, Exchange::MEMarketUpdate> QueuedMarketUpdates;
    QueuedMarketUpdates snapshot_queued_msgs_;
    QueuedMarketUpdates incremental_queued_msgs_;

  private:
    auto run() noexcept -> void;
    auto recvCallback(Common::McastSocket *socket) noexcept -> void;
    auto queueMessage(bool is_snapshot, const Exchange::MDPMarketUpdate *request) -> void;
    auto startSnapshotSync() -> void;
    auto checkSnapshotSync() -> void;
  };
}
