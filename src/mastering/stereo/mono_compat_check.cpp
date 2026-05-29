#include "mastering/stereo/mono_compat_check.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"
#include "util/stereo_metrics.h"

namespace sonare::mastering::stereo {
namespace {

using sonare::constants::kPiD;

}  // namespace

MonoCompatResult mono_compat_check(const float* left, const float* right, size_t length,
                                   float correlation_threshold) {
  if (length == 0 || left == nullptr || right == nullptr) {
    throw std::invalid_argument("mono compatibility buffers must not be empty or null");
  }

  MonoCompatResult result;
  result.correlation = util::stereo_correlation(left, right, length);
  result.width = util::stereo_width(left, right, length);

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

std::vector<MonoCompatBandResult> mono_compat_check_log_bands(const float* left, const float* right,
                                                              size_t length, double sample_rate,
                                                              int bands_per_octave, float low_hz,
                                                              float high_hz) {
  if (length == 0 || left == nullptr || right == nullptr) {
    throw std::invalid_argument("mono compatibility buffers must not be empty or null");
  }
  if (!(sample_rate > 0.0) || bands_per_octave <= 0 || !(low_hz > 0.0f) || !(high_hz > low_hz) ||
      high_hz >= static_cast<float>(sample_rate * 0.5)) {
    throw std::invalid_argument("invalid mono compatibility band configuration");
  }

  const double ratio = std::pow(2.0, 1.0 / static_cast<double>(bands_per_octave));
  std::vector<MonoCompatBandResult> result;
  for (double low = low_hz; low < high_hz; low *= ratio) {
    const double high = std::min(static_cast<double>(high_hz), low * ratio);
    const double center = std::sqrt(low * high);
    double left_re = 0.0;
    double left_im = 0.0;
    double right_re = 0.0;
    double right_im = 0.0;
    double side_sum = 0.0;
    for (size_t i = 0; i < length; ++i) {
      const double phase = 2.0 * kPiD * center * static_cast<double>(i) / sample_rate;
      const double c = std::cos(phase);
      const double s = std::sin(phase);
      left_re += left[i] * c;
      left_im += left[i] * s;
      right_re += right[i] * c;
      right_im += right[i] * s;
      const float side = 0.5f * (left[i] - right[i]);
      side_sum += static_cast<double>(side) * side;
    }
    const double left_energy = left_re * left_re + left_im * left_im;
    const double right_energy = right_re * right_re + right_im * right_im;
    float correlation = 0.0f;
    if (left_energy > 0.0 && right_energy > 0.0) {
      correlation = static_cast<float>((left_re * right_re + left_im * right_im) /
                                       std::sqrt(left_energy * right_energy));
    }
    result.push_back({static_cast<float>(low), static_cast<float>(high), correlation,
                      static_cast<float>(std::sqrt(side_sum / static_cast<double>(length)))});
  }
  return result;
}

}  // namespace sonare::mastering::stereo
