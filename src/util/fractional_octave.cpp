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
  for (size_t i = 1; i < values.size(); ++i) {
    const float center = frequencies[i];
    const float low = center / ratio;
    const float high = center * ratio;
    float sum = 0.0f;
    size_t count = 0;
    for (size_t j = 1; j < values.size(); ++j) {
      if (frequencies[j] >= low && frequencies[j] <= high) {
        sum += values[j];
        ++count;
      }
    }
    smoothed[i] = count == 0 ? values[i] : sum / static_cast<float>(count);
  }
  return smoothed;
}

}  // namespace sonare::util
