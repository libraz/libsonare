#include "mastering/stereo/mono_compat_check.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "analysis/meter/stereo.h"

namespace sonare::mastering::stereo {

MonoCompatResult mono_compat_check(const float* left, const float* right, size_t length,
                                   float correlation_threshold) {
  if (length == 0 || left == nullptr || right == nullptr) {
    throw std::invalid_argument("mono compatibility buffers must not be empty or null");
  }

  MonoCompatResult result;
  result.correlation = analysis::meter::correlation(left, right, length);
  result.width = analysis::meter::stereo_width(left, right, length);

  double side_sum = 0.0;
  for (size_t i = 0; i < length; ++i) {
    const float mono = 0.5f * (left[i] + right[i]);
    const float side = 0.5f * (left[i] - right[i]);
    result.mono_peak = std::max(result.mono_peak, std::abs(mono));
    side_sum += static_cast<double>(side) * side;
  }
  result.side_rms = static_cast<float>(std::sqrt(side_sum / static_cast<double>(length)));
  result.likely_mono_compatible = result.correlation >= correlation_threshold;
  return result;
}

}  // namespace sonare::mastering::stereo
