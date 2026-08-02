#include "metering/true_peak.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "rt/true_peak_fir.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::metering {

using sonare::constants::kEpsilon;
using sonare::constants::kFloorDb;

namespace {

float upsampled_peak(const float* data, size_t length, int oversample_factor) {
  const auto& pf = ::sonare::rt::true_peak_fir_for(oversample_factor);
  float peak = peak_abs(data, length);
  for (int phase = 0; phase < pf.phases; ++phase) {
    for (size_t i = 0; i < length; ++i) {
      const float sample = ::sonare::rt::interpolate_polyphase_sample(data, length, i, phase, pf);
      peak = std::max(peak, std::abs(sample));
    }
  }
  return peak;
}

}  // namespace

float true_peak(const float* data, size_t length, int oversample_factor) {
  SONARE_CHECK(oversample_factor >= 1, ErrorCode::InvalidParameter);
  // Reject factors without a dedicated polyphase design instead of silently
  // falling back to 4x and reporting a result mislabeled with the requested
  // factor. Supported factors: 1, 2, 4, 8, 16.
  SONARE_CHECK_MSG(::sonare::rt::is_supported_polyphase_oversample_factor(oversample_factor),
                   ErrorCode::InvalidParameter,
                   "true_peak oversample_factor must be one of 1, 2, 4, 8, 16");
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
  // Silence floor: report the shared finite dB floor (kFloorDb, -120 dB) rather
  // than -inf, matching the spectrum and dynamic-range meters. A finite floor is
  // JSON-safe and gives every level-in-dB meter the same "silence" sentinel.
  if (peak < kEpsilon) return kFloorDb;
  return linear_to_db(peak);
}

}  // namespace sonare::metering
