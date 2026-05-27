#pragma once

/// @file oversampler.h
/// @brief Lightweight offline oversampling helper.

#include <vector>

#include "rt/polyphase_fir.h"

namespace sonare::rt {

class Oversampler {
 public:
  explicit Oversampler(int factor = 2);

  void set_factor(int factor);
  int factor() const { return factor_; }
  int latency_samples() const noexcept { return fir_.taps_per_phase / 2; }

  std::vector<float> upsample(const float* input, size_t size) const;
  std::vector<float> upsample(const std::vector<float>& input) const;
  std::vector<float> downsample(const float* input, size_t size) const;
  std::vector<float> downsample(const std::vector<float>& input) const;
  void downsample_to(const float* input, size_t size, float* output, size_t output_size) const;

 private:
  int factor_ = 2;
  PolyphaseFir fir_;
  std::vector<float> decimation_taps_;
};

}  // namespace sonare::rt
