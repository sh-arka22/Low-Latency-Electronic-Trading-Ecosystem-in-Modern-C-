#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "common/logging.h"
#include "common/opt_logging.h"
#include "common/perf_utils.h"

static std::string randomString(size_t length) {
  static const char charset[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";
  constexpr size_t max_index = sizeof(charset) - 1;
  std::string s(length, '\0');
  std::generate_n(s.begin(), length, []() { return charset[rand() % max_index]; });
  return s;
}

template <typename T>
static size_t benchmarkLogging(T *logger) {
  constexpr size_t loop_count = 100000;
  size_t total_rdtsc = 0;
  for (size_t i = 0; i < loop_count; ++i) {
    const auto s     = randomString(128);
    const auto start = Common::rdtsc();
    logger->log("%\n", s);
    total_rdtsc += (Common::rdtsc() - start);
  }
  return total_rdtsc / loop_count;
}

int main(int, char **) {
  using namespace std::literals::chrono_literals;

  {
    Common::Logger logger("logger_benchmark_original.log");
    const auto cycles = benchmarkLogging(&logger);
    std::cout << "ORIGINAL  LOGGER " << cycles << " CLOCK CYCLES PER OPERATION." << std::endl;
    std::this_thread::sleep_for(10s);  // let the async logger drain
  }
  {
    OptCommon::OptLogger opt_logger("logger_benchmark_optimized.log");
    const auto cycles = benchmarkLogging(&opt_logger);
    std::cout << "OPTIMIZED LOGGER " << cycles << " CLOCK CYCLES PER OPERATION." << std::endl;
    std::this_thread::sleep_for(10s);
  }
  return EXIT_SUCCESS;
}
