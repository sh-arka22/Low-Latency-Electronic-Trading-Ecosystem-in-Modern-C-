#include <array>
#include <cstdlib>
#include <iostream>

#include "common/mem_pool.h"
#include "common/opt_mem_pool.h"
#include "common/perf_utils.h"
#include "market_data/market_update.h"

template <typename T>
static size_t benchmarkMemPool(T *mem_pool) {
  constexpr size_t loop_count = 100000;
  std::array<Exchange::MDPMarketUpdate *, 256> objs{};
  size_t total_rdtsc = 0;

  for (size_t i = 0; i < loop_count; ++i) {
    for (size_t j = 0; j < objs.size(); ++j) {
      const auto start = Common::rdtsc();
      objs[j] = mem_pool->allocate();
      total_rdtsc += (Common::rdtsc() - start);
    }
    for (size_t j = 0; j < objs.size(); ++j) {
      const auto start = Common::rdtsc();
      mem_pool->deallocate(objs[j]);
      total_rdtsc += (Common::rdtsc() - start);
    }
  }
  return total_rdtsc / (loop_count * objs.size() * 2);
}

int main(int, char **) {
  {
    Common::MemPool<Exchange::MDPMarketUpdate> pool(512);
    const auto cycles = benchmarkMemPool(&pool);
    std::cout << "ORIGINAL  MEMPOOL " << cycles << " CLOCK CYCLES PER OPERATION." << std::endl;
  }
  {
    OptCommon::OptMemPool<Exchange::MDPMarketUpdate> pool(512);
    const auto cycles = benchmarkMemPool(&pool);
    std::cout << "OPTIMIZED MEMPOOL " << cycles << " CLOCK CYCLES PER OPERATION." << std::endl;
  }
  return EXIT_SUCCESS;
}
