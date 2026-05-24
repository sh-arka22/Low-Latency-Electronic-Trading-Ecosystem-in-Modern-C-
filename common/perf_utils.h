#pragma once

#include <cstdint>

namespace Common {
  /// Read from the TSC register; returns elapsed CPU clock cycles.
  inline auto rdtsc() noexcept {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
  }
}

/// Start latency measurement using rdtsc(). Creates a variable called TAG in the local scope.
#define START_MEASURE(TAG) const auto TAG = Common::rdtsc()

/// End latency measurement using rdtsc(). Expects TAG to already exist in the local scope.
#define END_MEASURE(TAG, LOGGER)                                                                \
      do {                                                                                      \
        const auto end = Common::rdtsc();                                                       \
        LOGGER.log("% RDTSC " #TAG " %\n", Common::getCurrentTimeStr(&time_str_), (end - TAG)); \
      } while (false)

/// Same as END_MEASURE but also records the cycle delta into a
/// Common::LatencyHistogram. One rdtsc() shared between the log and the
/// histogram, so this is the same cost as END_MEASURE plus one record.
#define END_MEASURE_HIST(TAG, LOGGER, HIST)                                                     \
      do {                                                                                      \
        const auto end = Common::rdtsc();                                                       \
        const auto delta = (end - TAG);                                                         \
        LOGGER.log("% RDTSC " #TAG " %\n", Common::getCurrentTimeStr(&time_str_), delta);       \
        (HIST).record(static_cast<int64_t>(delta));                                             \
      } while (false)

/// Log a current wall-clock nanosecond timestamp at the call site.
#define TTT_MEASURE(TAG, LOGGER)                                                                \
      do {                                                                                      \
        const auto TAG = Common::getCurrentNanos();                                             \
        LOGGER.log("% TTT " #TAG " %\n", Common::getCurrentTimeStr(&time_str_), TAG);           \
      } while (false)
