#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <type_traits>
#include <vector>

#include "core/audio.h"
#include "mastering/common/loudness_measure.h"
#include "metering/lufs.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

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

// A static gain within this tolerance of the requested gain
// counts as fully applied. Shared by every loudness path so they all report
// `loudness_target_limited` off the same comparison.
inline constexpr float kLoudnessGainToleranceDb = 1.0e-4f;

// A loudness stage counts as having reached its target when the achieved
// integrated loudness lands within this many LU of it. The post-gain true-peak
// limiter reshapes the waveform, so the gated measurement moves slightly even
// where no gain reduction was needed.
inline constexpr float kLoudnessTargetToleranceLu = 0.5f;

// The static gain a loudness stage requests and the gain it applies.
struct LoudnessStageGain {
  // Gain landing the remeasured stage input on the target, re-gated at that gain
  // (metering::gain_to_integrated_lufs); NaN when the input is below the absolute gate.
  float requested_db = std::numeric_limits<float>::quiet_NaN();
  // requested_db bounded by the ceiling; 0 when nothing was requested.
  float applied_db = 0.0f;
};

// Bounds a static normalization gain. The gain may exceed the peak headroom
// toward the ceiling by at most @p max_limiter_gain_reduction_db, which is how
// deep the post-gain true-peak limiter is allowed to be driven. Clamping
// strictly at the headroom instead makes the target unreachable on
// peak-normalized material, whose headroom is ~0 dB however far away the target
// is, and leaves the limiter that exists to close that distance with nothing to
// do. A non-finite @p peak_db leaves the gain unbounded.
inline float bound_loudness_gain_db(float requested_gain_db, float ceiling_db, float peak_db,
                                    float max_limiter_gain_reduction_db) noexcept {
  if (!std::isfinite(peak_db)) {
    return requested_gain_db;
  }
  const float headroom_db = ceiling_db - peak_db;
  return std::min(requested_gain_db, headroom_db + std::max(max_limiter_gain_reduction_db, 0.0f));
}

// Solves and bounds the gain for an interleaved stage input whose true peak is
// @p peak_db.
inline LoudnessStageGain loudness_stage_gain(const float* interleaved, std::size_t frames,
                                             int channels, int sample_rate, float target_lufs,
                                             float ceiling_db, float peak_db,
                                             float max_limiter_gain_reduction_db) {
  const metering::LufsGainToTarget solved =
      metering::gain_to_integrated_lufs(interleaved, frames, channels, sample_rate, target_lufs);
  LoudnessStageGain gain;
  if (!std::isfinite(solved.measured_lufs)) {
    return gain;
  }
  gain.requested_db = solved.gain_db;
  gain.applied_db =
      bound_loudness_gain_db(solved.gain_db, ceiling_db, peak_db, max_limiter_gain_reduction_db);
  if (!numeric::finite(db_to_linear(gain.applied_db))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "loudness target must produce a finite linear gain");
  }
  return gain;
}

// Mono stage input: measures its true peak, then solves and bounds the gain.
inline LoudnessStageGain loudness_gain_db_with_ceiling(const std::vector<float>& samples,
                                                       int sample_rate, float target_lufs,
                                                       float ceiling_db, int true_peak_oversample,
                                                       float max_limiter_gain_reduction_db) {
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  const float peak_db =
      sonare::mastering::common::measure_true_peak_dbtp(audio, true_peak_oversample);
  return loudness_stage_gain(samples.data(), samples.size(), 1, sample_rate, target_lufs,
                             ceiling_db, peak_db, max_limiter_gain_reduction_db);
}

// True when a loudness stage did not deliver its target, so the reported output
// LUFS is the achieved value rather than the requested one. Two ways to miss:
// the static gain was clamped short of the requested gain, or the post-gain
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

// Measures the stereo true peak as the maximum across the two independent
// channels, reading the caller's planar buffers rather than copying each into an
// Audio: on a track-length master the two copies were the measurement's whole
// allocation cost. @p sample_rate is unused because the meter is rate-agnostic.
inline float stereo_true_peak_dbtp(const std::vector<float>& left, const std::vector<float>& right,
                                   int /*sample_rate*/, int true_peak_oversample) {
  if (left.size() != right.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
  }
  return sonare::mastering::common::measure_true_peak_dbtp_stereo_planar(
      left.data(), right.data(), left.size(), true_peak_oversample);
}

// Stereo stage input: measures its true peak, then solves and bounds the gain
// with BS.1770 channel summing.
inline LoudnessStageGain loudness_gain_db_with_ceiling(const std::vector<float>& left,
                                                       const std::vector<float>& right,
                                                       int sample_rate, float target_lufs,
                                                       float ceiling_db, int true_peak_oversample,
                                                       float max_limiter_gain_reduction_db) {
  const float peak_db = stereo_true_peak_dbtp(left, right, sample_rate, true_peak_oversample);
  const std::vector<float> interleaved = interleave_stereo(left, right);
  return loudness_stage_gain(interleaved.data(), left.size(), 2, sample_rate, target_lufs,
                             ceiling_db, peak_db, max_limiter_gain_reduction_db);
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

// The two rules a linked stereo repair is measured against. Neither is on a
// production path any more -- every repair carries its own stereo form -- and
// they stay because deleting them would delete the comparison, not dead weight.

// Runs @p repair independently on each channel in place (left, then right). The
// channels are separate buffers and the repair transforms are pure, so this is
// equivalent to repairing both in either interleaving.
inline void apply_independent_repair(std::vector<float>& left, std::vector<float>& right,
                                     int sample_rate,
                                     const std::function<Audio(const Audio&)>& repair) {
  apply_repair_in_place(left, sample_rate, repair);
  apply_repair_in_place(right, sample_rate, repair);
}

// Derives one gain curve from the mono mix and applies it to both channels. A
// downmix decides for material it cannot see: one-sided content is halved and an
// antiphase pair cancels, which is the reason the stereo forms do not use one.
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

// Hands both channels to a repair that is itself stereo-aware. What the channels
// share -- a linked detection, one gain mask, one trim range -- is the repair's
// decision, so nothing here reconstructs a shared one from two separate results.
// A template because the seven repairs return seven result types; the bodies are
// small and the alternative is repacking each into a pair at every call site.
template <typename StereoRepairFn>
inline void apply_stereo_repair(std::vector<float>& left, std::vector<float>& right,
                                int sample_rate, StereoRepairFn&& repair) {
  if (left.empty() || right.empty()) return;
  auto result = repair(Audio::from_buffer(left.data(), left.size(), sample_rate),
                       Audio::from_buffer(right.data(), right.size(), sample_rate));
  // trim_silence shortens both channels, so the result length is not the input
  // length; only the two channels agreeing with each other is invariant.
  if (result.left.size() != result.right.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo repair produced mismatched channel lengths");
  }
  left.assign(result.left.data(), result.left.data() + result.left.size());
  right.assign(result.right.data(), result.right.data() + result.right.size());
}

}  // namespace sonare::mastering::api::detail
