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

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
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

  /// Apply a Darwin THREAD_AFFINITY_POLICY hint to the current thread.
  /// IMPORTANT: This is a *hint*, not a hard pin. Threads sharing the same
  /// non-zero affinity_tag are *likely* to be co-located on a core that
  /// shares L2 cache; the kernel may still migrate. On non-Darwin platforms
  /// this is a no-op. On Linux, use setThreadCore() for a real pin.
  inline auto pinCurrentThreadDarwinHint(int affinity_tag) noexcept -> bool {
    #if defined(__APPLE__)
        thread_affinity_policy_data_t policy{ affinity_tag };
        const thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
        const auto kr = thread_policy_set(
            mach_thread, THREAD_AFFINITY_POLICY,
            reinterpret_cast<thread_policy_t>(&policy),
            THREAD_AFFINITY_POLICY_COUNT);
        return kr == KERN_SUCCESS;
    #else
        (void)affinity_tag;
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
