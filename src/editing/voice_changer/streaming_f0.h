#pragma once

/// @file streaming_f0.h
/// @brief Streaming time-domain YIN f0 tracker running on a decimated copy of the input.

#include "editing/voice_changer/voice_transform_types.h"

namespace sonare::editing::voice_changer {

/// Emits f0 decisions that lag the input; the lag is information only and adds no audio delay.
class StreamingF0Tracker {
 public:
  void prepare(double sr, int max_block, float f0_min_hz);
  void reset();
  /// @return Number of estimates finalised in this block (at most ceil(n / hop_input) + 1).
  int process(const float* in, int n, F0Estimate* out, int max_out) noexcept;
  /// @brief Information lag of the estimates; not part of the audible latency.
  int decision_lag_samples() const noexcept;
  int hop_input_samples() const noexcept;
};

}  // namespace sonare::editing::voice_changer
