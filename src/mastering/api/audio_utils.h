#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
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

// A static gain within this tolerance of the requested `target - current` gain
// counts as fully applied. Shared by every loudness path so they all report
// `loudness_target_limited` off the same comparison.
inline constexpr float kLoudnessGainToleranceDb = 1.0e-4f;

// A loudness stage counts as having reached its target when the achieved
// integrated loudness lands within this many LU of it. The post-gain true-peak
// limiter reshapes the waveform, so the gated measurement moves slightly even
// where no gain reduction was needed.
inline constexpr float kLoudnessTargetToleranceLu = 0.5f;

// Compute the LUFS-normalization gain (target - current). The static gain may
// exceed the peak headroom toward the ceiling by at most
// @p max_limiter_gain_reduction_db, which is how deep the post-gain true-peak
// limiter is allowed to be driven. Clamping strictly at the headroom instead
// makes the target unreachable on peak-normalized material, whose headroom is
// ~0 dB however far away the target is, and leaves the limiter that exists to
// close that distance with nothing to do. Returns 0 when the loudness
// measurement is non-finite (e.g. silence below the absolute gate).
inline float loudness_gain_db_with_ceiling(float current_lufs, float target_lufs, float ceiling_db,
                                           float peak_db, float max_limiter_gain_reduction_db) {
  if (!std::isfinite(current_lufs)) {
    return 0.0f;
  }
  float gain_db = target_lufs - current_lufs;
  if (std::isfinite(peak_db)) {
    const float headroom_db = ceiling_db - peak_db;
    gain_db = std::min(gain_db, headroom_db + std::max(max_limiter_gain_reduction_db, 0.0f));
  }
  return gain_db;
}

// Mono convenience wrapper: measures current LUFS and true peak from @p samples.
inline float loudness_gain_db_with_ceiling(const std::vector<float>& samples, int sample_rate,
                                           float target_lufs, float ceiling_db,
                                           int true_peak_oversample,
                                           float max_limiter_gain_reduction_db) {
  const float current_lufs =
      sonare::mastering::common::measure_lufs(samples.data(), samples.size(), sample_rate);
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  const float peak_db =
      sonare::mastering::common::measure_true_peak_dbtp(audio, true_peak_oversample);
  return loudness_gain_db_with_ceiling(current_lufs, target_lufs, ceiling_db, peak_db,
                                       max_limiter_gain_reduction_db);
}

// True when a loudness stage did not deliver its target, so the reported output
// LUFS is the achieved value rather than the requested one. Two ways to miss:
// the static gain was clamped short of `target - current`, or the post-gain
// true-peak limiter pulled the achieved loudness back below the target.
// @p achieved_lufs is the integrated loudness measured after that limiter.
inline bool loudness_target_was_limited(float requested_gain_db, float applied_gain_db,
                                        float target_lufs, float achieved_lufs) {
  if (!std::isfinite(requested_gain_db)) {
    return false;
  }
  if (applied_gain_db < requested_gain_db - kLoudnessGainToleranceDb) {
    return true;
  }
  return std::isfinite(achieved_lufs) && achieved_lufs < target_lufs - kLoudnessTargetToleranceLu;
}

// Measures the stereo true peak as the maximum across the two independent channels.
inline float stereo_true_peak_dbtp(const std::vector<float>& left, const std::vector<float>& right,
                                   int sample_rate, int true_peak_oversample) {
  Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
  Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
  return std::max(
      sonare::mastering::common::measure_true_peak_dbtp(left_audio, true_peak_oversample),
      sonare::mastering::common::measure_true_peak_dbtp(right_audio, true_peak_oversample));
}

// Stereo convenience wrapper: measures LUFS with BS.1770 channel summing and the true peak.
inline float loudness_gain_db_with_ceiling(const std::vector<float>& left,
                                           const std::vector<float>& right, int sample_rate,
                                           float target_lufs, float ceiling_db,
                                           int true_peak_oversample,
                                           float max_limiter_gain_reduction_db) {
  const float current_lufs = stereo_integrated_lufs(left, right, sample_rate);
  const float peak_db = stereo_true_peak_dbtp(left, right, sample_rate, true_peak_oversample);
  return loudness_gain_db_with_ceiling(current_lufs, target_lufs, ceiling_db, peak_db,
                                       max_limiter_gain_reduction_db);
}

// Applies an in-place per-buffer repair: builds an Audio view of @p data, runs
// @p repair, and writes the (possibly resized) result back. Type-erased through
// std::function on purpose — the repair chain has ~10 call sites, and a template
// would emit one copy of this body per distinct lambda, bloating the binary.
inline void apply_repair_in_place(std::vector<float>& data, int sample_rate,
                                  const std::function<Audio(const Audio&)>& repair) {
  Audio input = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio repaired = repair(input);
  data.assign(repaired.data(), repaired.data() + repaired.size());
}

// Runs @p repair independently on each channel in place (left, then right). The
// channels are separate buffers and the repair transforms are pure, so this is
// equivalent to repairing both in either interleaving.
inline void apply_independent_repair(std::vector<float>& left, std::vector<float>& right,
                                     int sample_rate,
                                     const std::function<Audio(const Audio&)>& repair) {
  apply_repair_in_place(left, sample_rate, repair);
  apply_repair_in_place(right, sample_rate, repair);
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
