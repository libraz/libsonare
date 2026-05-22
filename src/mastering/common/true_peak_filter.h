#pragma once

/// @file true_peak_filter.h
/// @brief True-peak interpolation helper with BS.1770 4x default coefficients.

#include <cstddef>
#include <vector>

#include "mastering/common/polyphase_fir.h"

namespace sonare::mastering::common {

class TruePeakFilter {
 public:
  explicit TruePeakFilter(int num_channels = 1, int factor = 4);

  float process(const float* const* input, int num_channels, int num_samples) const;
  void upsample(const float* const* input, float* const* output_oversampled, int num_channels,
                int num_samples) const;
  void upsample_with_history(const float* const* input, float* const* output_oversampled,
                             int num_channels, int num_samples,
                             std::vector<std::vector<float>>& history) const;

  int factor() const noexcept { return factor_; }
  int latency_samples() const noexcept { return fir_.taps_per_phase / 2; }

 private:
  int factor_ = 4;
  PolyphaseFir fir_;
};

}  // namespace sonare::mastering::common
