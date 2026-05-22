#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include "perf_utils.h"

namespace Common {
  typedef int64_t Nanos;

  constexpr Nanos NANO_TO_MICROS   = 1000;
  constexpr Nanos MICROS_TO_MILLIS = 1000;
  constexpr Nanos MILLIS_TO_SECS   = 1000;
  constexpr Nanos NANOS_TO_MILLIS  = NANO_TO_MICROS * MICROS_TO_MILLIS;
  constexpr Nanos NANOS_TO_SECS    = NANOS_TO_MILLIS * MILLIS_TO_SECS;

  inline auto getCurrentNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
  }

  /// Format current timestamp as HH:MM:SS.nnnnnnnnn.
  /// Note: string formatting is inefficient; this is for log files only, not hot paths.
  inline auto &getCurrentTimeStr(std::string *time_str) {
    const auto clock = std::chrono::system_clock::now();
    const auto time  = std::chrono::system_clock::to_time_t(clock);

    char nanos_str[24];
    // ctime() returns "Day Mon DD HH:MM:SS YYYY\n"; offset 11 skips to "HH:MM:SS".
    std::snprintf(nanos_str, sizeof(nanos_str), "%.8s.%09lld",
                  ctime(&time) + 11,
                  static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                      clock.time_since_epoch()).count() % NANOS_TO_SECS));
    time_str->assign(nanos_str);
    return *time_str;
  }
}
