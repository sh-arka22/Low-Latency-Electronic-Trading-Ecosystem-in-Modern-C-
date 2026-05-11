#include "market_data/snapshot_synthesizer.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "common/thread_utils.h"

namespace Exchange {
  SnapshotSynthesizer::SnapshotSynthesizer(MDPMarketUpdateLFQueue *market_updates,
                                           const std::string &iface,
                                           const std::string &snapshot_ip,
                                           int snapshot_port)
      : snapshot_md_updates_(market_updates),
        logger_("exchange_snapshot_synthesizer.log"),
        snapshot_socket_(logger_),
        order_pool_(ME_MAX_ORDER_IDS) {
    ASSERT(snapshot_socket_.init(snapshot_ip, iface, snapshot_port,
                                 /*is_listening*/ false) >= 0,
           "Unable to create snapshot mcast socket. error:" +
               std::string(std::strerror(errno)));

    for (auto &orders : ticker_orders_) orders.fill(nullptr);
  }

  SnapshotSynthesizer::~SnapshotSynthesizer() { stop(); }

  auto SnapshotSynthesizer::start() -> void {
    run_ = true;
    ASSERT(Common::createAndStartThread(-1, "Exchange/SnapshotSynthesizer",
                                        [this]() { run(); }) != nullptr,
           "Failed to start SnapshotSynthesizer thread.");
  }

  auto SnapshotSynthesizer::stop() -> void { run_ = false; }

  auto SnapshotSynthesizer::addToSnapshot(const MDPMarketUpdate *market_update)
      -> void {
    const auto &me      = market_update->me_market_update_;
    auto       *orders = &ticker_orders_.at(me.ticker_id_);

    switch (me.type_) {
      case MarketUpdateType::ADD: {
        ASSERT(orders->at(me.order_id_) == nullptr,
               "Received ADD for an existing order:" + me.toString());
        orders->at(me.order_id_) = order_pool_.allocate(me);
        break;
      }
      case MarketUpdateType::MODIFY: {
        auto *order = orders->at(me.order_id_);
        ASSERT(order != nullptr,
               "Received MODIFY for missing order:" + me.toString());
        ASSERT(order->order_id_ == me.order_id_,
               "OrderId mismatch on MODIFY:" + me.toString());
        order->qty_   = me.qty_;
        order->price_ = me.price_;
        break;
      }
      case MarketUpdateType::CANCEL: {
        auto *order = orders->at(me.order_id_);
        ASSERT(order != nullptr,
               "Received CANCEL for missing order:" + me.toString());
        order_pool_.deallocate(order);
        orders->at(me.order_id_) = nullptr;
        break;
      }
      case MarketUpdateType::SNAPSHOT_START:
      case MarketUpdateType::CLEAR:
      case MarketUpdateType::SNAPSHOT_END:
      case MarketUpdateType::TRADE:
      case MarketUpdateType::INVALID:
        break;
    }

    ASSERT(market_update->seq_num_ == last_inc_seq_num_ + 1,
           "Expected incremental seq_nums to increase by 1. Got:" +
               std::to_string(market_update->seq_num_) +
               " last:" + std::to_string(last_inc_seq_num_));
    last_inc_seq_num_ = market_update->seq_num_;
  }

  auto SnapshotSynthesizer::publishSnapshot() -> void {
    size_t snapshot_size = 0;

    // SNAPSHOT_START — order_id_ carries the latest incremental seq we built from.
    const MDPMarketUpdate start{
        snapshot_size++,
        MEMarketUpdate{MarketUpdateType::SNAPSHOT_START,
                       last_inc_seq_num_, TickerId_INVALID,
                       Side::INVALID, Price_INVALID, Qty_INVALID,
                       Priority_INVALID}};
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), start.toString());
    snapshot_socket_.send(&start, sizeof(MDPMarketUpdate));

    for (size_t ticker_id = 0; ticker_id < ticker_orders_.size(); ++ticker_id) {
      const auto &orders = ticker_orders_.at(ticker_id);

      MEMarketUpdate clear_me{};
      clear_me.type_      = MarketUpdateType::CLEAR;
      clear_me.ticker_id_ = static_cast<TickerId>(ticker_id);
      const MDPMarketUpdate clear{snapshot_size++, clear_me};
      logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), clear.toString());
      snapshot_socket_.send(&clear, sizeof(MDPMarketUpdate));

      for (const auto *order : orders) {
        if (order) {
          const MDPMarketUpdate upd{snapshot_size++, *order};
          logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__,
                      Common::getCurrentTimeStr(&time_str_), upd.toString());
          snapshot_socket_.send(&upd, sizeof(MDPMarketUpdate));
          snapshot_socket_.sendAndRecv();  // flush per order to avoid TX buf bloat
        }
      }
    }

    // SNAPSHOT_END — order_id_ again carries last_inc_seq_num_ so client can resume.
    const MDPMarketUpdate end{
        snapshot_size++,
        MEMarketUpdate{MarketUpdateType::SNAPSHOT_END,
                       last_inc_seq_num_, TickerId_INVALID,
                       Side::INVALID, Price_INVALID, Qty_INVALID,
                       Priority_INVALID}};
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), end.toString());
    snapshot_socket_.send(&end, sizeof(MDPMarketUpdate));
    snapshot_socket_.sendAndRecv();

    logger_.log("%:% %() % Published snapshot of % orders.\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), snapshot_size - 1);
  }

  auto SnapshotSynthesizer::run() -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
    while (run_) {
      for (auto market_update = snapshot_md_updates_->getNextToRead();
           snapshot_md_updates_->size() && market_update;
           market_update = snapshot_md_updates_->getNextToRead()) {
        logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__,
                    __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                    market_update->toString());
        addToSnapshot(market_update);
        snapshot_md_updates_->updateReadIndex();
      }

      if (Common::getCurrentNanos() - last_snapshot_time_ >
          60 * Common::NANOS_TO_SECS) {
        last_snapshot_time_ = Common::getCurrentNanos();
        publishSnapshot();
      }
    }
  }
}
