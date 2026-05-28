#pragma once

// VPIN — Volume-synchronized Probability of Informed Trading.
//
// Easley, López de Prado, O'Hara (2012): bulk-volume buckets of size V are
// formed sequentially as trades print. Within each bucket, the share of buy
// volume vs sell volume is classified by bulk-volume classification (BVC) —
// for simplicity we use the raw trade side here since the project's tape
// already carries explicit aggressor side (Side::BUY = buyer-initiated).
//
// VPIN within a window of N consecutive buckets is the average of
//     |buy_volume_i − sell_volume_i| / V
// over the N most recent buckets. Crypto baselines tend to settle ~0.45–0.47
// in quiet sessions; spikes above 0.55–0.65 indicate informed flow.

#include <array>
#include <cmath>
#include <cstdint>

namespace Trading {

  template <std::size_t kWindow>
  class Vpin {
  public:
    void setBucketSize(double bucket_size) noexcept {
      bucket_size_ = bucket_size;
    }

    void onTrade(double qty, bool is_buyer_initiated) noexcept {
      if (bucket_size_ <= 0.0) return;

      double remaining = qty;
      while (remaining > 0.0) {
        const double room = bucket_size_ - cur_filled_;
        const double take = std::min(room, remaining);
        if (is_buyer_initiated) cur_buy_  += take;
        else                    cur_sell_ += take;
        cur_filled_ += take;
        remaining   -= take;
        if (cur_filled_ >= bucket_size_) {
          closeBucket();
        }
      }
    }

    // Current VPIN; NaN until kWindow buckets have been seen.
    double value() const noexcept {
      if (buckets_seen_ < kWindow) return std::nan("");
      double s = 0.0;
      for (std::size_t i = 0; i < kWindow; ++i) s += imbalances_[i];
      return s / static_cast<double>(kWindow);
    }

    std::size_t bucketsSeen() const noexcept { return buckets_seen_; }

  private:
    void closeBucket() noexcept {
      const double imb = bucket_size_ > 0.0
          ? std::abs(cur_buy_ - cur_sell_) / bucket_size_
          : 0.0;
      imbalances_[head_] = imb;
      head_ = (head_ + 1) % kWindow;
      if (buckets_seen_ < kWindow * 2) ++buckets_seen_;
      cur_buy_ = cur_sell_ = cur_filled_ = 0.0;
    }

    double bucket_size_ = 0.0;
    double cur_buy_     = 0.0;
    double cur_sell_    = 0.0;
    double cur_filled_  = 0.0;

    std::array<double, kWindow> imbalances_{};
    std::size_t head_         = 0;
    std::size_t buckets_seen_ = 0;
  };

}
