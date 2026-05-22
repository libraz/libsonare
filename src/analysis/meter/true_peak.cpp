#include "analysis/meter/true_peak.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "mastering/common/polyphase_fir.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::analysis::meter {
namespace {

const ::sonare::mastering::common::PolyphaseFir& filter_for(int oversample_factor) {
  static const auto kFilter4x = ::sonare::mastering::common::design_polyphase_lowpass(4, 48);
  static const auto kFilter8x = ::sonare::mastering::common::design_polyphase_lowpass(8, 96);
  static const auto kFilter2x = ::sonare::mastering::common::design_polyphase_lowpass(2, 24);
  if (oversample_factor == 8) return kFilter8x;
  if (oversample_factor == 2) return kFilter2x;
  return kFilter4x;
}

float upsampled_peak(const float* data, size_t length, int oversample_factor) {
  const auto& pf = filter_for(oversample_factor);
  float peak = peak_abs(data, length);
  for (int phase = 1; phase < pf.phases; ++phase) {
    for (size_t i = 0; i < length; ++i) {
      const float sample =
          ::sonare::mastering::common::interpolate_polyphase_sample(data, length, i, phase, pf);
      peak = std::max(peak, std::abs(sample));
    }
  }
  return peak;
}

}  // namespace

float true_peak(const float* data, size_t length, int oversample_factor) {
  SONARE_CHECK(oversample_factor >= 1, ErrorCode::InvalidParameter);
  SONARE_CHECK(data != nullptr || length == 0, ErrorCode::InvalidParameter);
  if (length == 0) return 0.0f;

  if (oversample_factor == 1 || length < 2) {
    return peak_abs(data, length);
  }
  return upsampled_peak(data, length, oversample_factor);
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
