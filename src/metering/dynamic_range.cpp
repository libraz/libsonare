#include "metering/dynamic_range.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::metering {

using sonare::constants::kEpsilon;

namespace {

float rms_db_for_window(const float* data, size_t start, size_t length, float floor_db) {
  if (length == 0) return floor_db;

  const float window_rms = rms(data + start, length);
  if (window_rms < kEpsilon) return floor_db;
  return std::max(floor_db, linear_to_db(window_rms));
}

float percentile_sorted(const std::vector<float>& sorted, float percentile) {
  if (sorted.empty()) return 0.0f;
  const float position = std::clamp(percentile, 0.0f, 1.0f) * static_cast<float>(sorted.size() - 1);
  const size_t low = static_cast<size_t>(std::floor(position));
  const size_t high = static_cast<size_t>(std::ceil(position));
  if (low == high) return sorted[low];
  const float frac = position - static_cast<float>(low);
  return sorted[low] * (1.0f - frac) + sorted[high] * frac;
}

}  // namespace

DynamicRangeResult dynamic_range(const Audio& audio, const DynamicRangeConfig& config) {
  SONARE_CHECK(config.window_sec > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_sec > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.low_percentile >= 0.0f && config.low_percentile <= 1.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(config.high_percentile >= 0.0f && config.high_percentile <= 1.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(config.low_percentile <= config.high_percentile, ErrorCode::InvalidParameter);

  DynamicRangeResult result;
  if (audio.empty()) return result;

  const size_t window =
      std::max<size_t>(1, static_cast<size_t>(std::round(config.window_sec * audio.sample_rate())));
  const size_t hop =
      std::max<size_t>(1, static_cast<size_t>(std::round(config.hop_sec * audio.sample_rate())));
  const float* data = audio.data();

  if (audio.size() < window) {
    // The signal is shorter than a single analysis window: measure the whole
    // thing once so short clips still yield a defined result.
    result.window_rms_db.push_back(rms_db_for_window(data, 0, audio.size(), config.floor_db));
  } else {
    // Emit only complete windows. A trailing partial window is computed over its
    // own (shorter) length, so its RMS is not comparable to the full-length
    // windows and would skew the low/high percentiles.
    for (size_t start = 0; start + window <= audio.size(); start += hop) {
      result.window_rms_db.push_back(rms_db_for_window(data, start, window, config.floor_db));
    }
  }

  std::vector<float> sorted = result.window_rms_db;
  std::sort(sorted.begin(), sorted.end());
  result.low_percentile_db = percentile_sorted(sorted, config.low_percentile);
  result.high_percentile_db = percentile_sorted(sorted, config.high_percentile);
  result.dynamic_range_db = result.high_percentile_db - result.low_percentile_db;
  return result;
}

}  // namespace sonare::metering
