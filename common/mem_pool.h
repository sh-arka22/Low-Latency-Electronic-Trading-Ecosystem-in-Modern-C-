#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/macros.h"

namespace Common {
  /// Pre-allocated, fixed-capacity object pool. allocate()/deallocate() are O(1)
  /// amortized. Zero heap traffic on the hot path.
  template<typename T>
  class MemPool final {
  public:
    explicit MemPool(std::size_t num_elems)
        : store_(num_elems, {T(), true}) {
      ASSERT(reinterpret_cast<const ObjectBlock *>(&(store_[0].object_)) == &(store_[0]),
             "T object must be first member of ObjectBlock.");
    }

    template<typename... Args>
    T *allocate(Args... args) noexcept {
      auto block = &(store_[next_free_index_]);
      ASSERT(block->is_free_,
             "Expected free ObjectBlock at index:" + std::to_string(next_free_index_));
      T *ret = &(block->object_);
      ret = new(ret) T(args...);  // placement new
      block->is_free_ = false;
      updateNextFreeIndex();
      return ret;
    }

    auto deallocate(const T *elem) noexcept {
      const auto idx = reinterpret_cast<const ObjectBlock *>(elem) - &store_[0];
      ASSERT(idx >= 0 && static_cast<size_t>(idx) < store_.size(),
             "Element being deallocated does not belong to this Memory pool.");
      ASSERT(!store_[idx].is_free_,
             "Expected in-use ObjectBlock at index:" + std::to_string(idx));
      store_[idx].is_free_ = true;
    }

    MemPool() = delete;
    MemPool(const MemPool &) = delete;
    MemPool(const MemPool &&) = delete;
    MemPool &operator=(const MemPool &) = delete;
    MemPool &operator=(const MemPool &&) = delete;

  private:
    auto updateNextFreeIndex() noexcept {
      const auto initial = next_free_index_;
      while (!store_[next_free_index_].is_free_) {
        ++next_free_index_;
        if (UNLIKELY(next_free_index_ == store_.size())) {
          next_free_index_ = 0;
        }
        if (UNLIKELY(initial == next_free_index_)) {
          ASSERT(initial != next_free_index_, "Memory Pool out of space.");
        }
      }
    }

    struct ObjectBlock {
      T    object_;
      bool is_free_ = true;
    };

    std::vector<ObjectBlock> store_;
    size_t                   next_free_index_ = 0;
  };
}
