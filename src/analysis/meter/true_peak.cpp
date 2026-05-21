#include "analysis/meter/true_peak.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::analysis::meter {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kSincRadius = 16;

float sinc(float x) {
  if (std::abs(x) < 1e-6f) return 1.0f;
  const float pix = kPi * x;
  return std::sin(pix) / pix;
}

float hann_window(float distance) {
  if (distance >= static_cast<float>(kSincRadius)) return 0.0f;
  return 0.5f + 0.5f * std::cos(kPi * distance / static_cast<float>(kSincRadius));
}

float interpolate_sinc(const float* data, size_t length, float position) {
  const int center = static_cast<int>(std::floor(position));
  double sum = 0.0;
  double weight_sum = 0.0;
  for (int k = center - kSincRadius + 1; k <= center + kSincRadius; ++k) {
    if (k < 0 || k >= static_cast<int>(length)) continue;
    const float distance = position - static_cast<float>(k);
    const float weight = sinc(distance) * hann_window(std::abs(distance));
    sum += static_cast<double>(data[k]) * weight;
    weight_sum += weight;
  }
  if (std::abs(weight_sum) < 1e-12) return 0.0f;
  return static_cast<float>(sum / weight_sum);
}

}  // namespace

float true_peak(const float* data, size_t length, int oversample_factor) {
  SONARE_CHECK(oversample_factor >= 1, ErrorCode::InvalidParameter);
  SONARE_CHECK(data != nullptr || length == 0, ErrorCode::InvalidParameter);
  if (length == 0) return 0.0f;

  float peak = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    peak = std::max(peak, std::abs(data[i]));
  }

  if (oversample_factor == 1 || length < 2) return peak;

  for (size_t i = 0; i + 1 < length; ++i) {
    for (int phase = 1; phase < oversample_factor; ++phase) {
      const float t = static_cast<float>(phase) / static_cast<float>(oversample_factor);
      const float interpolated = interpolate_sinc(data, length, static_cast<float>(i) + t);
      peak = std::max(peak, std::abs(interpolated));
    }
  }

  return peak;
}

float true_peak(const Audio& audio, int oversample_factor) {
  return true_peak(audio.data(), audio.size(), oversample_factor);
}

float true_peak_db(const Audio& audio, int oversample_factor) {
  const float peak = true_peak(audio, oversample_factor);
  if (peak < kEpsilon) return -std::numeric_limits<float>::infinity();
  return 20.0f * std::log10(peak);
}

}  // namespace sonare::analysis::meter
