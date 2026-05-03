#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace Common {
  /// Pin the current thread to the given core. Linux-only; on other OSes this
  /// is a no-op so the rest of the system still runs (just without affinity).
  inline auto setThreadCore(int core_id) noexcept -> bool {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)core_id;
    return true;
#endif
  }

  /// Spawn a named thread, optionally pin it to a core, and run func(args...).
  /// Sleeps briefly so the new thread is up before returning.
  template<typename T, typename... A>
  inline auto createAndStartThread(int core_id, const std::string &name,
                                   T &&func, A &&... args) noexcept {
    auto t = new std::thread([&]() {
      if (core_id >= 0 && !setThreadCore(core_id)) {
        std::cerr << "Failed to set core affinity for " << name << " "
                  << pthread_self() << " to " << core_id << std::endl;
        std::exit(EXIT_FAILURE);
      }
      std::cerr << "Set core affinity for " << name << " "
                << pthread_self() << " to " << core_id << std::endl;

      std::forward<T>(func)((std::forward<A>(args))...);
    });

    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);
    return t;
  }
}
