#pragma once

// Hand-rolled log2-bucket latency histogram. Single-writer; no atomics, no
// locks. One instance per thread. Story is percentiles, not the specific
// library — see PERF.md. Drop-in interface for HdrHistogram_c if we ever swap.

#include <array>
#include <climits>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace Common {

  class LatencyHistogram final {
  public:
    explicit LatencyHistogram(std::string name) : name_(std::move(name)) {}

    /// Record one observation. value in whatever unit the caller picks
    /// (cycles, ns — caller is responsible for consistency).
    auto record(int64_t value) noexcept -> void {
      if (value < 0) value = 0;
      if (value > max_) max_ = value;
      if (value < min_) min_ = value;
      sum_ += static_cast<uint64_t>(value);
      ++count_;
      if (value == 0) { ++buckets_[0]; return; }
      // Bucket i covers [2^i, 2^(i+1)). __builtin_clzll undefined on 0 — handled above.
      const int bucket = 63 - __builtin_clzll(static_cast<uint64_t>(value));
      ++buckets_[bucket];
    }

    auto count() const noexcept -> uint64_t { return count_; }
    auto min()   const noexcept -> int64_t  { return count_ ? min_ : 0; }
    auto max()   const noexcept -> int64_t  { return max_; }
    auto mean()  const noexcept -> double {
      return count_ ? static_cast<double>(sum_) / count_ : 0.0;
    }
    auto name()  const noexcept -> const std::string & { return name_; }

    /// Approximate value at percentile p in [0.0, 1.0]. Linear interpolation
    /// within the chosen bucket; accuracy bound is the bucket width (≈2×).
    auto percentile(double p) const noexcept -> int64_t {
      if (count_ == 0) return 0;
      if (p < 0.0) p = 0.0;
      if (p > 1.0) p = 1.0;
      const uint64_t target = static_cast<uint64_t>(p * count_);
      uint64_t cumulative = 0;
      for (int i = 0; i < 64; ++i) {
        cumulative += buckets_[i];
        if (cumulative >= target) {
          if (i == 0) return 0;
          const uint64_t low  = 1ULL << i;
          const uint64_t high = 1ULL << (i + 1);
          const uint64_t prev = cumulative - buckets_[i];
          const double frac = buckets_[i] > 0
              ? static_cast<double>(target - prev) / buckets_[i]
              : 0.0;
          return static_cast<int64_t>(low + frac * (high - low));
        }
      }
      return max_;
    }

    /// Write tag header, percentile summary, then non-empty buckets as CSV.
    auto dumpCsv(const std::string &path) const -> void {
      std::ofstream f(path);
      if (!f.is_open()) {
        std::cerr << "LatencyHistogram::dumpCsv failed to open " << path << '\n';
        return;
      }
      f << "# tag,"   << name_  << '\n';
      f << "# count," << count_ << '\n';
      f << "# min,"   << min()  << '\n';
      f << "# max,"   << max_   << '\n';
      f << "# mean,"  << mean() << '\n';
      f << "# p50,"   << percentile(0.50)   << '\n';
      f << "# p90,"   << percentile(0.90)   << '\n';
      f << "# p99,"   << percentile(0.99)   << '\n';
      f << "# p999,"  << percentile(0.999)  << '\n';
      f << "# p9999," << percentile(0.9999) << '\n';
      f << "bucket_low,bucket_high,count\n";
      for (int i = 0; i < 64; ++i) {
        if (buckets_[i] == 0) continue;
        const uint64_t low  = (i == 0) ? 0ULL : (1ULL << i);
        const uint64_t high = 1ULL << (i + 1);
        f << low << ',' << high << ',' << buckets_[i] << '\n';
      }
    }

    LatencyHistogram()                                    = delete;
    LatencyHistogram(const LatencyHistogram &)            = delete;
    LatencyHistogram(LatencyHistogram &&)                 = delete;
    LatencyHistogram &operator=(const LatencyHistogram &) = delete;
    LatencyHistogram &operator=(LatencyHistogram &&)      = delete;

  private:
    const std::string         name_;
    std::array<uint64_t, 64>  buckets_{};
    uint64_t                  count_ = 0;
    uint64_t                  sum_   = 0;
    int64_t                   min_   = INT64_MAX;
    int64_t                   max_   = 0;
  };

}  // namespace Common
