#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "mastering/common/loudness_measure.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::api::detail {

inline std::vector<float> mono_mix(const std::vector<float>& left,
                                   const std::vector<float>& right) {
  if (left.size() != right.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
  }
  std::vector<float> mono(left.size());
  for (std::size_t index = 0; index < left.size(); ++index) {
    mono[index] = 0.5f * (left[index] + right[index]);
  }
  return mono;
}

inline void apply_gain_db(std::vector<float>& samples, float gain_db) {
  const float gain = db_to_linear(gain_db);
  for (float& sample : samples) {
    sample *= gain;
  }
}

inline void apply_gain_db(std::vector<float>& left, std::vector<float>& right, float gain_db) {
  if (left.size() != right.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
  }
  const float gain = db_to_linear(gain_db);
  for (std::size_t index = 0; index < left.size(); ++index) {
    left[index] *= gain;
    right[index] *= gain;
  }
}

// Compute the LUFS-normalization gain (target - current), clamped so the static
// gain does not push the measured true peak past the ceiling. Mirrors the mono
// `loudness_optimize()` helper so that the chain / stereo loudness stages do not
// overdrive the downstream true-peak limiter into distortion. Returns 0 when the
// loudness measurement is non-finite (e.g. silence below the absolute gate).
inline float loudness_gain_db_with_ceiling(float current_lufs, float target_lufs, float ceiling_db,
                                           float peak_db) {
  if (!std::isfinite(current_lufs)) {
    return 0.0f;
  }
  float gain_db = target_lufs - current_lufs;
  if (std::isfinite(peak_db)) {
    gain_db = std::min(gain_db, ceiling_db - peak_db);
  }
  return gain_db;
}

// Mono convenience wrapper: measures current LUFS and true peak from @p samples.
inline float loudness_gain_db_with_ceiling(const std::vector<float>& samples, int sample_rate,
                                           float target_lufs, float ceiling_db,
                                           int true_peak_oversample) {
  const float current_lufs =
      sonare::mastering::common::measure_lufs(samples.data(), samples.size(), sample_rate);
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  const float peak_db =
      sonare::mastering::common::measure_true_peak_dbtp(audio, true_peak_oversample);
  return loudness_gain_db_with_ceiling(current_lufs, target_lufs, ceiling_db, peak_db);
}

// Stereo convenience wrapper: measures LUFS on the mono mix and the true peak as
// the max across the two channels (mirrors the chain's per-channel max metric).
inline float loudness_gain_db_with_ceiling(const std::vector<float>& left,
                                           const std::vector<float>& right, int sample_rate,
                                           float target_lufs, float ceiling_db,
                                           int true_peak_oversample) {
  std::vector<float> mono = mono_mix(left, right);
  const float current_lufs =
      sonare::mastering::common::measure_lufs(mono.data(), mono.size(), sample_rate);
  Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
  Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
  const float peak_db = std::max(
      sonare::mastering::common::measure_true_peak_dbtp(left_audio, true_peak_oversample),
      sonare::mastering::common::measure_true_peak_dbtp(right_audio, true_peak_oversample));
  return loudness_gain_db_with_ceiling(current_lufs, target_lufs, ceiling_db, peak_db);
}

}  // namespace sonare::mastering::api::detail
