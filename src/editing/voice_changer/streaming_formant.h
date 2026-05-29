#pragma once

/// @file streaming_formant.h
/// @brief Low-latency streaming formant color shaper for realtime voice changer.

#include <array>

#include "rt/biquad_design.h"

namespace sonare::editing::voice_changer {

struct StreamingFormantConfig {
  float factor = 1.0f;
  float amount = 1.0f;
  float body = 0.0f;
  float brightness = 0.0f;
  float nasal = 0.0f;
};

class StreamingFormant {
 public:
  explicit StreamingFormant(StreamingFormantConfig config = {});

  void prepare(double sample_rate, int max_block_size);
  void reset();
  void set_config(const StreamingFormantConfig& config);
  const StreamingFormantConfig& config() const noexcept { return config_; }
  void process_block(const float* input, float* output, int num_samples) noexcept;

 private:
  void update_filters() noexcept;

  StreamingFormantConfig config_{};
  /// @brief Set by @ref prepare. Zero means "not prepared" so @ref process_block
  ///        is a safe no-op until the host calls prepare() with a positive rate.
  ///        Initializing to a non-zero default (48000.0) caused
  ///        @ref update_filters to compute biquads for the wrong sample rate if
  ///        the host used set_config() before prepare().
  double sample_rate_ = 0.0;
  int max_block_size_ = 0;
  std::array<rt::BiquadState, 4> filters_{};
  float smoothed_factor_ = 1.0f;
  float filter_factor_ = 1.0f;
  float factor_alpha_ = 1.0f;
  /// @brief Counts down to the next coefficient rebuild across blocks. Used to
  ///        be a per-block local variable, which reset the cadence at every
  ///        block boundary and effectively forced a rebuild on sample 0 of
  ///        every block (the opposite of the intended 32-sample interval).
  int filter_update_countdown_ = 0;
};

}  // namespace sonare::editing::voice_changer
