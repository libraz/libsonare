#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>
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

inline std::vector<float> interleave_stereo(const std::vector<float>& left,
                                            const std::vector<float>& right) {
  if (left.size() != right.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
  }
  std::vector<float> interleaved(left.size() * 2);
  for (std::size_t index = 0; index < left.size(); ++index) {
    interleaved[2 * index] = left[index];
    interleaved[2 * index + 1] = right[index];
  }
  return interleaved;
}

inline float stereo_integrated_lufs(const std::vector<float>& left, const std::vector<float>& right,
                                    int sample_rate) {
  const std::vector<float> interleaved = interleave_stereo(left, right);
  return sonare::mastering::common::measure_lufs_interleaved(interleaved.data(), left.size(), 2,
                                                             sample_rate);
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

// Stereo convenience wrapper: measures LUFS with BS.1770 channel summing and the
// true peak as the max across the two channels.
inline float loudness_gain_db_with_ceiling(const std::vector<float>& left,
                                           const std::vector<float>& right, int sample_rate,
                                           float target_lufs, float ceiling_db,
                                           int true_peak_oversample) {
  const float current_lufs = stereo_integrated_lufs(left, right, sample_rate);
  Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
  Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
  const float peak_db = std::max(
      sonare::mastering::common::measure_true_peak_dbtp(left_audio, true_peak_oversample),
      sonare::mastering::common::measure_true_peak_dbtp(right_audio, true_peak_oversample));
  return loudness_gain_db_with_ceiling(current_lufs, target_lufs, ceiling_db, peak_db);
}

template <typename RepairFn>
inline void apply_shared_mono_transfer_repair(std::vector<float>& left, std::vector<float>& right,
                                              int sample_rate, RepairFn&& repair) {
  static_assert(std::is_invocable_r_v<sonare::Audio, RepairFn, const sonare::Audio&>,
                "repair must accept const Audio& and return Audio");
  std::vector<float> mono = mono_mix(left, right);
  if (mono.empty()) return;

  const Audio mono_audio = Audio::from_buffer(mono.data(), mono.size(), sample_rate);
  const Audio repaired_audio = repair(mono_audio);
  if (repaired_audio.size() != mono.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "shared stereo repair produced mismatched length");
  }

  // Spectral repairs shift zero crossings, so the per-sample ratio is unbounded
  // where the mono mix passes through zero while the repaired output does not.
  // Bound the transfer magnitude; within the bound the signed ratio is exact.
  constexpr float kEpsilon = 1.0e-6f;
  constexpr float kMaxTransferGain = 4.0f;
  for (std::size_t index = 0; index < mono.size(); ++index) {
    const float in = mono[index];
    const float out = repaired_audio[index];
    float gain = 1.0f;
    if (std::abs(in) > kEpsilon) {
      gain = out / in;
    }
    if (!std::isfinite(gain)) {
      gain = 1.0f;
    }
    gain = std::clamp(gain, -kMaxTransferGain, kMaxTransferGain);
    left[index] *= gain;
    right[index] *= gain;
  }
}

}  // namespace sonare::mastering::api::detail
