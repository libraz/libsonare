#pragma once

/// @file oversampler.h
/// @brief Lightweight offline oversampling helper.

#include <vector>

namespace sonare::mastering::common {

class Oversampler {
 public:
  explicit Oversampler(int factor = 2);

  void set_factor(int factor);
  int factor() const { return factor_; }

  std::vector<float> upsample(const float* input, size_t size) const;
  std::vector<float> upsample(const std::vector<float>& input) const;
  std::vector<float> downsample(const float* input, size_t size) const;
  std::vector<float> downsample(const std::vector<float>& input) const;

 private:
  int factor_ = 2;
};

}  // namespace sonare::mastering::common
