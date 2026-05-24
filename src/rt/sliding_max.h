#pragma once

/// @file sliding_max.h
/// @brief O(1) amortized sliding-window maximum.

#include <cstddef>
#include <deque>
#include <utility>

namespace sonare::rt {

template <typename T>
class SlidingMax {
 public:
  explicit SlidingMax(size_t window_size = 1) { prepare(window_size); }

  void prepare(size_t window_size) {
    window_ = window_size == 0 ? 1 : window_size;
    reset();
  }

  void push(T value) {
    while (!dq_.empty() && dq_.back().second <= value) {
      dq_.pop_back();
    }
    dq_.emplace_back(next_index_, value);
    ++next_index_;

    const size_t first_valid = next_index_ > window_ ? next_index_ - window_ : 0;
    while (!dq_.empty() && dq_.front().first < first_valid) {
      dq_.pop_front();
    }
  }

  T max() const noexcept { return dq_.empty() ? T{} : dq_.front().second; }

  void reset() {
    dq_.clear();
    next_index_ = 0;
  }

 private:
  size_t window_ = 1;
  size_t next_index_ = 0;
  std::deque<std::pair<size_t, T>> dq_;
};

}  // namespace sonare::rt
