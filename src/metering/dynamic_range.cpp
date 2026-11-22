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
#include "util/parameter_checks.h"

namespace sonare::metering {

namespace {

float rms_db_for_window(const float* data, size_t start, size_t length) {
  return amplitude_to_level_db(rms(data + start, length));
}

}  // namespace

void validate_dynamic_range_config(const DynamicRangeConfig& config) {
  namespace check = util::check;
  check::greater_than(config.window_sec, 0.0, "window_sec");
  check::greater_than(config.hop_sec, 0.0, "hop_sec");
  check::in_range(config.low_percentile, 0.0, 1.0, "low_percentile");
  check::in_range(config.high_percentile, 0.0, 1.0, "high_percentile");
  if (config.low_percentile > config.high_percentile) {
    check::refuse_relation({"low_percentile", "high_percentile"}, "{0} must be at most {1}");
  }
}

DynamicRangeConfig dynamic_range_config_from_public(float window_sec, float hop_sec,
                                                    float low_percentile, float high_percentile) {
  namespace check = util::check;
  check::at_least(window_sec, 0.0, "window_sec");
  check::at_least(hop_sec, 0.0, "hop_sec");
  check::finite_float(low_percentile, "low_percentile");
  check::finite_float(high_percentile, "high_percentile");

  DynamicRangeConfig config;
  if (window_sec > 0.0f) config.window_sec = window_sec;
  if (hop_sec > 0.0f) config.hop_sec = hop_sec;
  if (low_percentile >= 0.0f) config.low_percentile = low_percentile;
  if (high_percentile >= 0.0f) config.high_percentile = high_percentile;
  validate_dynamic_range_config(config);
  return config;
}

DynamicRangeResult dynamic_range(const Audio& audio, const DynamicRangeConfig& config) {
  validate_dynamic_range_config(config);

  DynamicRangeResult result;
  if (audio.empty()) {
    result.low_percentile_db = kNotMeasured;
    result.high_percentile_db = kNotMeasured;
    result.dynamic_range_db = kNotMeasured;
    return result;
  }

  // A non-finite sample would put a NaN level into the std::sort below.
  SONARE_CHECK_MSG(numeric::all_finite(audio.data(), audio.size()), ErrorCode::InvalidParameter,
                   "dynamic_range: audio contains a non-finite sample");

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
    result.window_rms_db.push_back(rms_db_for_window(data, 0, audio.size()));
  } else {
    // Emit only complete windows. A trailing partial window is computed over its
    // own (shorter) length, so its RMS is not comparable to the full-length
    // windows and would skew the low/high percentiles.
    const size_t last_start = audio.size() - window;
    for (size_t start = 0;;) {
      result.window_rms_db.push_back(rms_db_for_window(data, start, window));
      if (hop > last_start - start) break;
      start += hop;
    }
  }

  std::vector<float> sorted = result.window_rms_db;
  std::sort(sorted.begin(), sorted.end());
  result.low_percentile_db = static_cast<float>(percentile_sorted(sorted, config.low_percentile));
  result.high_percentile_db = static_cast<float>(percentile_sorted(sorted, config.high_percentile));
  result.dynamic_range_db = result.high_percentile_db == kNoLevelDb
                                ? kNotMeasured
                                : result.high_percentile_db - result.low_percentile_db;
  return result;
}

}  // namespace sonare::metering
