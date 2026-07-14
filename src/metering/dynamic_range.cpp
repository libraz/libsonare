#include "metering/dynamic_range.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"

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
  SONARE_CHECK(numeric::finite_positive(config.window_sec), ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_positive(config.hop_sec), ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_in_closed_range(config.low_percentile, 0.0f, 1.0f),
               ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_in_closed_range(config.high_percentile, 0.0f, 1.0f),
               ErrorCode::InvalidParameter);
  SONARE_CHECK(config.low_percentile <= config.high_percentile, ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.floor_db), ErrorCode::InvalidParameter);

  DynamicRangeResult result;
  if (audio.empty()) return result;

  size_t window = 0;
  size_t hop = 0;
  SONARE_CHECK(numeric::checked_round_cast(
                   static_cast<double>(config.window_sec) * audio.sample_rate(), &window) &&
                   window > 0,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::checked_round_cast(
                   static_cast<double>(config.hop_sec) * audio.sample_rate(), &hop) &&
                   hop > 0,
               ErrorCode::InvalidParameter);
  const float* data = audio.data();

  if (audio.size() < window) {
    // The signal is shorter than a single analysis window: measure the whole
    // thing once so short clips still yield a defined result.
    result.window_rms_db.push_back(rms_db_for_window(data, 0, audio.size(), config.floor_db));
  } else {
    // Emit only complete windows. A trailing partial window is computed over its
    // own (shorter) length, so its RMS is not comparable to the full-length
    // windows and would skew the low/high percentiles.
    const size_t last_start = audio.size() - window;
    for (size_t start = 0;;) {
      result.window_rms_db.push_back(rms_db_for_window(data, start, window, config.floor_db));
      if (hop > last_start - start) break;
      start += hop;
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
