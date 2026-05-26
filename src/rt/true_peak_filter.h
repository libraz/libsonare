#pragma once

/// @file true_peak_filter.h
/// @brief True-peak interpolation helper using self-designed polyphase FIRs
///        (2x / 4x / 8x) matching the BS.1770 target frequency response.

#include <cstddef>
#include <vector>

#include "rt/polyphase_fir.h"

namespace sonare::rt {

class TruePeakFilter {
 public:
  explicit TruePeakFilter(int num_channels = 1, int factor = 4);

  /// Pre-size the member history/scratch buffers for up to @p num_channels
  /// channels and @p max_block_size samples per call. Non-RT: call once before
  /// the audio thread uses the internal-history upsample overload, after which
  /// that path performs no allocation for any channel count / block size within
  /// the prepared bounds.
  void prepare(int num_channels, int max_block_size);

  float process(const float* const* input, int num_channels, int num_samples) const;
  void upsample(const float* const* input, float* const* output_oversampled, int num_channels,
                int num_samples) const;
  /// Internal-history overload: uses the member history + scratch buffers sized
  /// by prepare(). RT-safe (no allocation) once prepared for >= num_channels and
  /// >= num_samples.
  void upsample_with_history(const float* const* input, float* const* output_oversampled,
                             int num_channels, int num_samples) const;
  void upsample_with_history(const float* const* input, float* const* output_oversampled,
                             int num_channels, int num_samples,
                             std::vector<std::vector<float>>& history) const;
  void upsample_with_history(const float* const* input, float* const* output_oversampled,
                             int num_channels, int num_samples,
                             std::vector<std::vector<float>>& history,
                             std::vector<std::vector<float>>& scratch) const;

  int factor() const noexcept { return factor_; }
  int latency_samples() const noexcept { return fir_.taps_per_phase / 2; }

 private:
  int factor_ = 4;
  PolyphaseFir fir_;

  // Member-backed history + scratch for the internal-history overload so the
  // audio thread does not allocate per call. Mutable because the public methods
  // are const; the buffers only cache working storage (scratch) and per-channel
  // tail history. Sized by prepare().
  mutable std::vector<std::vector<float>> internal_history_;
  mutable std::vector<std::vector<float>> internal_scratch_;
};

}  // namespace sonare::rt
