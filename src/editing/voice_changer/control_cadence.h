#pragma once

/// @file control_cadence.h
/// @brief Shared absolute-sample cadence for realtime control updates.

#include <cstdint>

namespace sonare::editing::voice_changer {

/// @brief Smoothing time of every live-parameter smoother, in milliseconds.
inline constexpr float kParamSmoothingMs = 12.0f;

/// @brief Fires on sample 0 and every 32 absolute samples thereafter.
///
/// The counter is stateful so a block boundary cannot move a control update.
/// Resetting a DSP stage is the only operation that moves the first update
/// point back to sample 0.
class ControlCadence {
 public:
  static constexpr std::uint64_t kIntervalSamples = 32;

  void reset() noexcept { sample_position_ = 0; }

  /// @brief Advances one sample and reports whether its controls should update.
  bool advance() noexcept {
    const bool due = is_due(sample_position_);
    ++sample_position_;
    return due;
  }

  /// @brief Tests an absolute sample position without changing the counter.
  static constexpr bool is_due(std::uint64_t sample_position) noexcept {
    return (sample_position & (kIntervalSamples - 1)) == 0;
  }

  /// @brief Number of samples consumed since the last reset.
  std::uint64_t sample_position() const noexcept { return sample_position_; }

 private:
  std::uint64_t sample_position_ = 0;
};

}  // namespace sonare::editing::voice_changer
