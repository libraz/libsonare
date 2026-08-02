#pragma once

/// @file oversampler.h
/// @brief Lightweight offline oversampling helper.

#include <vector>

#include "rt/polyphase_fir.h"

namespace sonare::rt {

class Oversampler {
 public:
  /// Per-channel state for continuous oversampling. Prepare this on the control
  /// thread, then reuse it for every audio block in one channel's stream.
  struct StreamingState {
    std::vector<float> up_history;
    std::vector<float> up_scratch;
    std::vector<float> down_history;
    std::vector<float> down_scratch;
  };

  explicit Oversampler(int factor = 2, int taps_per_phase = 12);

  void set_factor(int factor);
  int factor() const { return factor_; }
  int latency_samples() const noexcept { return fir_.taps_per_phase / 2; }

  std::vector<float> upsample(const float* input, size_t size) const;
  std::vector<float> upsample(const std::vector<float>& input) const;
  std::vector<float> downsample(const float* input, size_t size) const;
  std::vector<float> downsample(const std::vector<float>& input) const;
  /// @brief Allocation-free upsample into a caller-provided buffer.
  /// @details @p output must hold at least @c size*factor() samples. Lets the
  ///          audio thread reuse preallocated scratch instead of allocating a
  ///          fresh vector per block.
  void upsample_to(const float* input, size_t size, float* output, size_t output_size) const;
  void downsample_to(const float* input, size_t size, float* output, size_t output_size) const;

  /// Allocate a state for continuous blocks up to @p max_input_samples. The
  /// streaming methods perform no allocation while their inputs stay in range.
  void prepare_streaming(StreamingState* state, size_t max_input_samples) const;
  void reset_streaming(StreamingState* state) const noexcept;
  /// Stateful counterparts of upsample_to()/downsample_to(). They delay their
  /// respective outputs by one FIR group delay so every emitted sample has its
  /// complete centered stencil, including across a preceding block boundary.
  void upsample_to_streaming(const float* input, size_t size, float* output, size_t output_size,
                             StreamingState* state) const;
  void downsample_to_streaming(const float* input, size_t size, float* output, size_t output_size,
                               StreamingState* state) const;
  /// Total base-rate delay of a streaming upsample/nonlinear/downsample path.
  int streaming_round_trip_latency_samples() const noexcept {
    return factor_ == 1 ? 0 : 2 * latency_samples();
  }

 private:
  int factor_ = 2;
  int taps_per_phase_ = 12;
  PolyphaseFir fir_;
  std::vector<float> decimation_taps_;
};

}  // namespace sonare::rt
