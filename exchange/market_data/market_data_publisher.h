#pragma once

#include <chrono>
#include <string>
#include <thread>

#include "common/lf_queue.h"
#include "common/logging.h"
#include "common/macros.h"
#include "common/mcast_socket.h"
#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "common/types.h"
#include "market_data/market_update.h"
#include "market_data/snapshot_synthesizer.h"

namespace Exchange {
  /// Drains MEMarketUpdate from the matching engine, multicasts each one as an
  /// MDPMarketUpdate on the incremental UDP stream, and also forwards every
  /// update to the SnapshotSynthesizer thread via snapshot_md_updates_.
  class MarketDataPublisher final {
  public:
    MarketDataPublisher(MEMarketUpdateLFQueue *market_updates,
                        const std::string &iface,
                        const std::string &snapshot_ip, int snapshot_port,
                        const std::string &incremental_ip, int incremental_port);

    /// Spin up this thread + the snapshot synthesizer thread.
    auto start() {
      run_ = true;
      ASSERT(Common::createAndStartThread(-1, "Exchange/MarketDataPublisher",
                                          [this]() { run(); }) != nullptr,
             "Failed to start MarketDataPublisher thread.");
      snapshot_synthesizer_->start();
    }

    auto stop() -> void {
      run_ = false;
      if (snapshot_synthesizer_) snapshot_synthesizer_->stop();
    }

    ~MarketDataPublisher() {
      stop();
      using namespace std::literals::chrono_literals;
      std::this_thread::sleep_for(5s);  // let in-flight work drain
      delete snapshot_synthesizer_;
      snapshot_synthesizer_ = nullptr;
    }

    auto run() noexcept -> void;

    MarketDataPublisher() = delete;
    MarketDataPublisher(const MarketDataPublisher &) = delete;
    MarketDataPublisher(const MarketDataPublisher &&) = delete;
    MarketDataPublisher &operator=(const MarketDataPublisher &) = delete;
    MarketDataPublisher &operator=(const MarketDataPublisher &&) = delete;

  private:
    size_t next_inc_seq_num_ = 1;  // monotonic counter for the incremental feed

    MEMarketUpdateLFQueue  *outgoing_md_updates_ = nullptr;  // not owned
    MDPMarketUpdateLFQueue  snapshot_md_updates_;             // owned (value)

    volatile bool run_ = false;
    std::string   time_str_;
    Common::Logger logger_;

    Common::McastSocket   incremental_socket_;
    SnapshotSynthesizer  *snapshot_synthesizer_ = nullptr;  // owned
  };
}
