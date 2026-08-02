/// @file fractional_octave.cpp
/// @brief Implementation of the fractional-octave smoothing utility.

#include "util/fractional_octave.h"

#include <cmath>

#include "util/exception.h"

namespace sonare::util {

std::vector<float> smooth_fractional_octave(const std::vector<float>& values,
                                            const std::vector<float>& frequencies,
                                            int octave_fraction) {
  SONARE_CHECK(octave_fraction > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(values.size() == frequencies.size(), ErrorCode::InvalidParameter);

  std::vector<float> smoothed(values.size(), 0.0f);
  if (values.empty()) return smoothed;

  smoothed[0] = values[0];
  const float ratio = std::pow(2.0f, 1.0f / (2.0f * static_cast<float>(octave_fraction)));
  // frequencies is monotonic by contract, so both band edges only advance. A
  // prefix sum makes each window average O(1) instead of re-scanning all bins.
  std::vector<double> prefix(values.size() + 1, 0.0);
  for (size_t i = 0; i < values.size(); ++i) {
    prefix[i + 1] = prefix[i] + static_cast<double>(values[i]);
  }

  size_t begin = 1;
  size_t end = 1;
  for (size_t i = 1; i < values.size(); ++i) {
    const float center = frequencies[i];
    const float low = center / ratio;
    const float high = center * ratio;
    while (begin < values.size() && frequencies[begin] < low) ++begin;
    if (end < begin) end = begin;
    while (end < values.size() && frequencies[end] <= high) ++end;
    const size_t count = end - begin;
    smoothed[i] =
        count == 0 ? values[i]
                   : static_cast<float>((prefix[end] - prefix[begin]) / static_cast<double>(count));
  }
  return smoothed;
}

}  // namespace sonare::util
