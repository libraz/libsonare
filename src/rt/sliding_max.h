#pragma once

/// @file sliding_max.h
/// @brief O(1) amortized sliding-window maximum.

#include <cstddef>
#include <utility>
#include <vector>

namespace sonare::rt {

/// @brief Sliding-window maximum with allocation-free push()/max().
///
/// Implements a monotonic deque backed by a fixed-capacity circular buffer.
/// All heap allocation happens in prepare(); push(), max(), and reset() never
/// allocate, making them safe to call from the audio thread.
template <typename T>
class SlidingMax {
 public:
  explicit SlidingMax(size_t window_size = 1) { prepare(window_size); }

  void prepare(size_t window_size) {
    window_ = window_size == 0 ? 1 : window_size;
    // After eviction the deque holds at most window_ live entries, but during
    // push() it transiently holds up to window_ + 1 (the new entry is appended
    // before stale front entries are evicted). Allocate one further slot so a
    // full ring (head == tail meaning "empty") is never mistaken for that
    // transient peak: capacity = (window_ + 1) entries + 1 sentinel slot.
    buffer_.assign(window_ + 2, std::pair<size_t, T>{});
    reset();
  }

  void push(T value) {
    // Drop entries no greater than the incoming value (newest wins on ties).
    while (head_ != tail_ && back().second <= value) {
      tail_ = dec(tail_);
    }
    buffer_[tail_] = std::pair<size_t, T>(next_index_, value);
    tail_ = inc(tail_);
    ++next_index_;

    // Evict entries that have slid out of the window.
    const size_t first_valid = next_index_ > window_ ? next_index_ - window_ : 0;
    while (head_ != tail_ && buffer_[head_].first < first_valid) {
      head_ = inc(head_);
    }
  }

  T max() const noexcept { return head_ == tail_ ? T{} : buffer_[head_].second; }

  void reset() {
    head_ = 0;
    tail_ = 0;
    next_index_ = 0;
  }

 private:
  size_t inc(size_t i) const noexcept { return i + 1 == buffer_.size() ? 0 : i + 1; }
  size_t dec(size_t i) const noexcept { return i == 0 ? buffer_.size() - 1 : i - 1; }

  // Most recently pushed entry; valid only when head_ != tail_.
  const std::pair<size_t, T>& back() const noexcept { return buffer_[dec(tail_)]; }

  size_t window_ = 1;
  size_t next_index_ = 0;
  size_t head_ = 0;  // index of the front (oldest) entry
  size_t tail_ = 0;  // one-past the back (newest) entry
  std::vector<std::pair<size_t, T>> buffer_;
};

}  // namespace sonare::rt
