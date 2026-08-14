#pragma once

/// @file delay_line.h
/// @brief Fixed-size mono delay line.

#include <cstddef>
#include <vector>

namespace sonare::rt {

class DelayLine {
 public:
  void prepare(size_t delay_samples);
  void reset() noexcept;
  float process(float input) noexcept;
  size_t delay_samples() const noexcept { return delay_samples_; }

  /// Number of samples physically reserved by this delay line. A zero-delay
  /// line intentionally reserves nothing, so the RT pass-through path carries
  /// no per-lane scratch allocation.
  size_t capacity() const noexcept { return buffer_.capacity(); }
  size_t reserved_samples() const noexcept { return buffer_.capacity(); }

 private:
  std::vector<float> buffer_{};
  size_t delay_samples_ = 0;
  size_t write_index_ = 0;
};

}  // namespace sonare::rt
