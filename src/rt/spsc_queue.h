#pragma once

/// @file spsc_queue.h
/// @brief Wait-free single-producer/single-consumer ring buffer.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <vector>

#include "util/exception.h"

namespace sonare::rt {

template <typename T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "SpscQueue records must be trivially copyable");

 public:
  SpscQueue() = default;
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;

  /// Allocates storage for exactly @p capacity_pow2 records. Non-RT only.
  void reserve(size_t capacity_pow2) {
    if (capacity_pow2 == 0 || (capacity_pow2 & (capacity_pow2 - 1)) != 0) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "SpscQueue capacity must be a non-zero power of two");
    }
    buffer_.assign(capacity_pow2, T{});
    mask_ = capacity_pow2 - 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  bool push(const T& item) noexcept {
    // A default-constructed queue has capacity 0: push/pop silently no-op,
    // which would drop records (e.g. telemetry) without any signal. Catch the
    // missing reserve() in debug builds; release builds still fail closed.
    assert(capacity() != 0 && "SpscQueue::push before reserve(): records will be dropped");
    // Explicit fail-closed guard for the never-reserved (capacity 0) queue so
    // release builds never index into an empty buffer_. Returning false also
    // makes the dropped-record behaviour observable to callers via the return
    // value rather than relying on the head/tail comparison below.
    if (capacity() == 0) {
      return false;
    }
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= capacity()) {
      return false;
    }

    buffer_[head & mask_] = item;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& out) noexcept {
    assert(capacity() != 0 && "SpscQueue::pop before reserve()");
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }

    out = buffer_[tail & mask_];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  size_t size_approx() const noexcept {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
  }

  size_t capacity() const noexcept { return buffer_.size(); }
  bool empty() const noexcept { return size_approx() == 0; }

 private:
  std::vector<T> buffer_;
  size_t mask_ = 0;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace sonare::rt
