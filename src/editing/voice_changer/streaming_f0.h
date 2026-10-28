#pragma once

/// @file streaming_f0.h
/// @brief Streaming time-domain YIN f0 tracker running on a decimated copy of the input.

#include <cstdint>
#include <vector>

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

 private:
  void push_decimated(float y) noexcept;
  F0Estimate analyse(std::int64_t centre_sample) noexcept;
  float lag_to_hz(float lag) const noexcept;

  double analysis_rate_ = 0.0;
  int hop_analysis_ = 0;
  int hop_input_ = 0;
  int tau_min_ = 0;
  int tau_max_ = 0;  ///< Longest searched lag, also the integration window, in analysis samples.
  int decision_lag_ = 0;

  // Decimation FIR (unity DC gain) over a mirrored history, so a dot product never wraps.
  std::vector<float> fir_taps_;
  std::vector<float> fir_history_;
  int fir_pos_ = 0;

  // Decimated signal; slot k & ring_mask_ holds analysis sample k.
  std::vector<float> ring_;
  std::int64_t ring_mask_ = 0;
  std::int64_t decimated_count_ = 0;

  std::vector<float> frame_;
  std::vector<float> diff_;
  std::vector<float> cmndf_;

  std::int64_t input_count_ = 0;
  bool voiced_ = false;
  int voiced_run_ = 0;
  int unvoiced_run_ = 0;
  float previous_lag_ = 0.0f;
};

}  // namespace sonare::editing::voice_changer
