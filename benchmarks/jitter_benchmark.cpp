// Day 6 — measure tight-loop rdtsc-to-rdtsc jitter on this machine.
//
// Pure CPU loop: read TSC, record the delta to the previous TSC, repeat.
// Most iterations come out at the no-op cost (~10–30 cycles). Anything
// bigger is a scheduler/OS event displacing the thread. The percentiles
// (p99 / p99.9 / p99.99) quantify the worst-case displacement on this
// macOS host.
//
// Usage:
//   ./jitter_benchmark unpinned [seconds=5] [out=docs/jitter_unpinned.hgrm]
//   ./jitter_benchmark pinned   [seconds=5] [out=docs/jitter_pinned.hgrm]
//
// The "pinned" mode applies pinCurrentThreadDarwinHint(1) — Darwin's
// THREAD_AFFINITY_POLICY hint. It's not a hard pin (the kernel may still
// migrate); we measure the empirical difference and report it honestly.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/latency_histogram.h"
#include "common/perf_utils.h"
#include "common/thread_utils.h"
#include "common/time_utils.h"

int main(int argc, char **argv) {
  std::string mode = (argc > 1) ? argv[1] : "unpinned";
  const int    secs = (argc > 2) ? std::atoi(argv[2]) : 5;
  const std::string out =
      (argc > 3) ? argv[3]
                 : (mode == "pinned" ? "jitter_pinned.hgrm"
                                     : "jitter_unpinned.hgrm");

  if (mode != "pinned" && mode != "unpinned") {
    std::fprintf(stderr, "Unknown mode '%s' (want 'pinned' or 'unpinned').\n",
                 mode.c_str());
    return 1;
  }

  if (mode == "pinned") {
    const bool ok = Common::pinCurrentThreadDarwinHint(1);
    std::fprintf(stderr, "[jitter] Darwin affinity hint: %s\n",
                 ok ? "applied" : "failed (continuing unpinned)");
  } else {
    std::fprintf(stderr, "[jitter] no affinity hint applied\n");
  }

  Common::LatencyHistogram hist(mode);

  const auto budget_ns = static_cast<int64_t>(secs) * 1000LL * 1000LL * 1000LL;
  const auto start_ns  = Common::getCurrentNanos();

  uint64_t prev = Common::rdtsc();
  uint64_t iter = 0;
  while (true) {
    const auto now = Common::rdtsc();
    hist.record(static_cast<int64_t>(now - prev));
    prev = now;
    if ((++iter & ((1u << 20) - 1)) == 0) {  // check time every 1M iters
      if (Common::getCurrentNanos() - start_ns >= budget_ns) break;
    }
  }

  std::fprintf(stderr,
               "[jitter] mode=%s count=%llu  p50=%lld  p99=%lld  p999=%lld  "
               "p9999=%lld  max=%lld\n",
               mode.c_str(),
               static_cast<unsigned long long>(hist.count()),
               static_cast<long long>(hist.percentile(0.50)),
               static_cast<long long>(hist.percentile(0.99)),
               static_cast<long long>(hist.percentile(0.999)),
               static_cast<long long>(hist.percentile(0.9999)),
               static_cast<long long>(hist.max()));

  hist.dumpCsv(out);
  std::fprintf(stderr, "[jitter] wrote %s\n", out.c_str());
  return 0;
}
