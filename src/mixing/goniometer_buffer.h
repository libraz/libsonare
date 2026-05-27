#pragma once

/// @file goniometer_buffer.h
/// @brief Small lock-free-style ring for decimated stereo scope points.

#include <array>
#include <atomic>
#include <cstddef>

namespace sonare::mixing {

struct GoniometerPoint {
  float left = 0.0f;
  float right = 0.0f;
};

template <size_t Capacity>
class GoniometerBuffer {
 public:
  static_assert(Capacity > 0, "Capacity must be positive");

  void push(float left, float right) noexcept {
    const size_t index = write_index_.load(std::memory_order_relaxed) % Capacity;
    points_[index] = {left, right};
    write_index_.store(write_index_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  void reset() noexcept {
    points_ = {};
    write_index_.store(0, std::memory_order_release);
  }

  size_t read_latest(GoniometerPoint* dest, size_t max_points) const noexcept {
    if (dest == nullptr || max_points == 0) {
      return 0;
    }
    const size_t written = write_index_.load(std::memory_order_acquire);
    const size_t count = written < Capacity ? written : Capacity;
    const size_t out_count = count < max_points ? count : max_points;
    const size_t start = written > out_count ? written - out_count : 0;
    for (size_t i = 0; i < out_count; ++i) {
      dest[i] = points_[(start + i) % Capacity];
    }
    return out_count;
  }

 private:
  std::array<GoniometerPoint, Capacity> points_{};
  std::atomic<size_t> write_index_{0};
};

}  // namespace sonare::mixing
