#include "market_data/market_data_consumer.h"

#include <cerrno>
#include <cstring>
#include <vector>

namespace Trading {
  MarketDataConsumer::MarketDataConsumer(Common::ClientId client_id,
                                         Exchange::MEMarketUpdateLFQueue *market_updates,
                                         const std::string &iface,
                                         const std::string &snapshot_ip, int snapshot_port,
                                         const std::string &incremental_ip, int incremental_port)
      : incoming_md_updates_(market_updates),
        run_(false),
        logger_("trading_market_data_consumer_" + std::to_string(client_id) + ".log"),
        incremental_mcast_socket_(logger_),
        snapshot_mcast_socket_(logger_),
        iface_(iface),
        snapshot_ip_(snapshot_ip),
        snapshot_port_(snapshot_port) {

    auto recv_callback = [this](auto socket) { recvCallback(socket); };

    // Incremental: init + join immediately. We listen to incrementals from start.
    incremental_mcast_socket_.recv_callback_ = recv_callback;
    ASSERT(incremental_mcast_socket_.init(incremental_ip, iface, incremental_port,
                                          /*is_listening*/ true) >= 0,
           "Unable to create incremental mcast socket. error:" +
               std::string(std::strerror(errno)));
    ASSERT(incremental_mcast_socket_.join(incremental_ip),
           "Join failed on:" + std::to_string(incremental_mcast_socket_.socket_fd_) +
               " error:" + std::string(std::strerror(errno)));

    // Snapshot: callback wired but socket NOT initialised — startSnapshotSync()
    // does that on demand when a gap is detected.
    snapshot_mcast_socket_.recv_callback_ = recv_callback;
  }

  // --------------------------------------------------------------------
  // Main loop. Two sendAndRecv()s — snapshot is a no-op when not joined.
  // All decoding happens in recvCallback() dispatched from sendAndRecv().
  // --------------------------------------------------------------------
  auto MarketDataConsumer::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_));
    while (run_) {
      incremental_mcast_socket_.sendAndRecv();
      snapshot_mcast_socket_.sendAndRecv();
    }
  }

  // --------------------------------------------------------------------
  // Receive dispatch. Distinguishes the two streams by fd comparison
  // (never by pointer — see Chapter 8 invariants). Walks the receive buffer
  // in chunks of sizeof(MDPMarketUpdate), drives the gap-detection state
  // machine, and either forwards in-sequence incrementals to the LFQueue
  // or buffers for later replay via queueMessage().
  // --------------------------------------------------------------------
  auto MarketDataConsumer::recvCallback(Common::McastSocket *socket) noexcept -> void {
    const auto is_snapshot = (socket->socket_fd_ == snapshot_mcast_socket_.socket_fd_);

    // Defensive: snapshot data arriving when we're not in recovery is unexpected.
    // Discard the buffer and log a warning. Should never happen because the
    // socket is leave()'d at the end of recovery.
    if (UNLIKELY(is_snapshot && !in_recovery_)) {
      socket->next_rcv_valid_index_ = 0;
      logger_.log("%:% %() % WARN Not expecting snapshot messages.\n",
                  __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_));
      return;
    }

    if (socket->next_rcv_valid_index_ >= sizeof(Exchange::MDPMarketUpdate)) {
      size_t i = 0;
      for (; i + sizeof(Exchange::MDPMarketUpdate) <= socket->next_rcv_valid_index_;
           i += sizeof(Exchange::MDPMarketUpdate)) {
        auto request = reinterpret_cast<const Exchange::MDPMarketUpdate *>(
            socket->inbound_data_.data() + i);

        // Gap detection: once latched into recovery, every subsequent message
        // (snapshot or incremental) goes through the buffered path until
        // checkSnapshotSync() clears the flag.
        const bool already_in_recovery = in_recovery_;
        in_recovery_ = (already_in_recovery ||
                        request->seq_num_ != next_exp_inc_seq_num_);

        if (UNLIKELY(in_recovery_)) {
          if (UNLIKELY(!already_in_recovery)) {
            // Edge transition into recovery: subscribe to the snapshot stream.
            logger_.log("%:% %() % Gap detected — entering recovery. exp:% got:%\n",
                        __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_),
                        next_exp_inc_seq_num_, request->seq_num_);
            startSnapshotSync();
          }
          queueMessage(is_snapshot, request);
        } else if (!is_snapshot) {
          // Normal happy path: incremental message with matching seq_num.
          ++next_exp_inc_seq_num_;
          auto next_write = incoming_md_updates_->getNextToWriteTo();
          *next_write = std::move(request->me_market_update_);
          incoming_md_updates_->updateWriteIndex();
        }
      }
      // Slide any trailing partial message left in the buffer.
      memcpy(socket->inbound_data_.data(),
             socket->inbound_data_.data() + i,
             socket->next_rcv_valid_index_ - i);
      socket->next_rcv_valid_index_ -= i;
    }
  }

  // --------------------------------------------------------------------
  // First step of recovery: clear any stale buffered messages, then
  // init() + join() the snapshot socket. Done on every entry to recovery;
  // checkSnapshotSync() tears the socket back down when we exit.
  // --------------------------------------------------------------------
  auto MarketDataConsumer::startSnapshotSync() -> void {
    snapshot_queued_msgs_.clear();
    incremental_queued_msgs_.clear();

    ASSERT(snapshot_mcast_socket_.init(snapshot_ip_, iface_, snapshot_port_,
                                       /*is_listening*/ true) >= 0,
           "Unable to create snapshot mcast socket. error:" +
               std::string(std::strerror(errno)));
    ASSERT(snapshot_mcast_socket_.join(snapshot_ip_),
           "Join failed on:" + std::to_string(snapshot_mcast_socket_.socket_fd_) +
               " error:" + std::string(std::strerror(errno)));
  }

  // --------------------------------------------------------------------
  // Buffer a single message into the appropriate recovery queue.
  // If the same snapshot seq_num is seen twice it means a new snapshot
  // cycle started before the previous one fully arrived — discard the
  // partial cycle and start fresh.
  // --------------------------------------------------------------------
  auto MarketDataConsumer::queueMessage(bool is_snapshot,
                                        const Exchange::MDPMarketUpdate *request) -> void {
    if (is_snapshot) {
      if (snapshot_queued_msgs_.find(request->seq_num_) != snapshot_queued_msgs_.end()) {
        logger_.log("%:% %() % Snapshot cycle restart at seq:%\n",
                    __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), request->seq_num_);
        snapshot_queued_msgs_.clear();
      }
      snapshot_queued_msgs_[request->seq_num_] = request->me_market_update_;
    } else {
      incremental_queued_msgs_[request->seq_num_] = request->me_market_update_;
    }

    checkSnapshotSync();
  }

  // --------------------------------------------------------------------
  // Try to assemble a complete recovery sequence. Three-phase algorithm:
  //   A. Validate the snapshot queue: must run START..END contiguously
  //      with no gaps in seq_num.
  //   B. Validate the incrementals: drop ones already covered by the
  //      snapshot, require contiguous seq_nums from SNAPSHOT_END.order_id_+1.
  //   C. Commit the merged event list to the trade-engine LFQueue, clear
  //      buffers, leave the snapshot multicast group, exit recovery.
  // Any validation failure exits without crashing — we just wait for more
  // data on the next sendAndRecv() round.
  // --------------------------------------------------------------------
  auto MarketDataConsumer::checkSnapshotSync() -> void {
    if (snapshot_queued_msgs_.empty()) return;

    // ===== Phase A: validate snapshot queue =====
    const auto &first_snapshot_msg = snapshot_queued_msgs_.begin()->second;
    if (first_snapshot_msg.type_ != Exchange::MarketUpdateType::SNAPSHOT_START) {
      logger_.log("%:% %() % Missing SNAPSHOT_START; discarding snapshot queue.\n",
                  __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_));
      snapshot_queued_msgs_.clear();
      return;
    }

    std::vector<Exchange::MEMarketUpdate> final_events;
    auto   have_complete_snapshot = true;
    size_t next_snapshot_seq      = 0;

    for (auto &snapshot_itr : snapshot_queued_msgs_) {
      if (snapshot_itr.first != next_snapshot_seq) {
        have_complete_snapshot = false;
        break;
      }
      if (snapshot_itr.second.type_ != Exchange::MarketUpdateType::SNAPSHOT_START &&
          snapshot_itr.second.type_ != Exchange::MarketUpdateType::SNAPSHOT_END) {
        final_events.push_back(snapshot_itr.second);
      }
      ++next_snapshot_seq;
    }
    if (!have_complete_snapshot) {
      logger_.log("%:% %() % Gap in snapshot queue; discarding.\n",
                  __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_));
      snapshot_queued_msgs_.clear();
      return;
    }

    const auto &last_snapshot_msg = snapshot_queued_msgs_.rbegin()->second;
    if (last_snapshot_msg.type_ != Exchange::MarketUpdateType::SNAPSHOT_END) {
      // Snapshot still in flight — wait for more messages.
      return;
    }

    // ===== Phase B: validate incrementals =====
    // SNAPSHOT_END.order_id_ carries the highest incremental seq covered by
    // the snapshot. We resume incremental processing from order_id_ + 1.
    auto   have_complete_incremental = true;
    size_t num_incrementals          = 0;
    next_exp_inc_seq_num_ = last_snapshot_msg.order_id_ + 1;

    for (auto inc_itr = incremental_queued_msgs_.begin();
         inc_itr != incremental_queued_msgs_.end(); ++inc_itr) {
      if (inc_itr->first < next_exp_inc_seq_num_) continue;
      if (inc_itr->first != next_exp_inc_seq_num_) {
        have_complete_incremental = false;
        break;
      }
      if (inc_itr->second.type_ != Exchange::MarketUpdateType::SNAPSHOT_START &&
          inc_itr->second.type_ != Exchange::MarketUpdateType::SNAPSHOT_END) {
        final_events.push_back(inc_itr->second);
      }
      ++next_exp_inc_seq_num_;
      ++num_incrementals;
    }
    if (!have_complete_incremental) {
      logger_.log("%:% %() % Gap in incrementals during recovery; discarding.\n",
                  __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_));
      snapshot_queued_msgs_.clear();
      return;
    }

    // ===== Phase C: commit + exit recovery =====
    for (const auto &itr : final_events) {
      auto next_write = incoming_md_updates_->getNextToWriteTo();
      *next_write = itr;
      incoming_md_updates_->updateWriteIndex();
    }

    logger_.log("%:% %() % Recovery complete. snapshot:% incrementals:% next_seq:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_),
                snapshot_queued_msgs_.size(), num_incrementals,
                next_exp_inc_seq_num_);

    snapshot_queued_msgs_.clear();
    incremental_queued_msgs_.clear();
    in_recovery_ = false;

    snapshot_mcast_socket_.leave(snapshot_ip_, snapshot_port_);
  }
}
