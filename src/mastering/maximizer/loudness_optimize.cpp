#include "mastering/maximizer/loudness_optimize.h"

#include <cmath>
#include <utility>
#include <vector>

#include "mastering/api/audio_utils.h"
#include "mastering/api/internal_processor_runner.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/common/parameter_domain.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/zero_is_default.h"

namespace sonare::mastering::maximizer {

float validate_loudness_params(float target_lufs, float ceiling_db, float release_ms,
                               float max_limiter_gain_reduction_db, int true_peak_oversample) {
  SONARE_CHECK_MSG(std::isfinite(target_lufs), ErrorCode::InvalidParameter,
                   "target_lufs must be finite");
  SONARE_CHECK_MSG(common::valid_ceiling_db(ceiling_db), ErrorCode::InvalidParameter,
                   "ceiling_db must be finite and <= 0");
  SONARE_CHECK_MSG(std::isfinite(release_ms) && release_ms >= 0.0f, ErrorCode::InvalidParameter,
                   "release_ms must be 0 (the library default) or a finite positive value");
  SONARE_CHECK_MSG(
      std::isfinite(max_limiter_gain_reduction_db) && max_limiter_gain_reduction_db >= 0.0f,
      ErrorCode::InvalidParameter, "max_limiter_gain_reduction_db must be finite and >= 0");
  SONARE_CHECK_MSG(true_peak_oversample == 1 || true_peak_oversample == 2 ||
                       true_peak_oversample == 4 || true_peak_oversample == 8 ||
                       true_peak_oversample == 16,
                   ErrorCode::InvalidParameter, "oversample must be one of 1, 2, 4, 8, or 16");
  // Checked above, so the sentinel is the only value that resolves to anything
  // other than itself.
  return ZeroIsDefault(release_ms).or_default(kDefaultLoudnessReleaseMs);
}

LoudnessOptimizeResult loudness_optimize(const Audio& audio, const LoudnessOptimizeConfig& config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const float release_ms =
      validate_loudness_params(config.target_lufs, config.ceiling_db, config.release_ms,
                               config.max_limiter_gain_reduction_db, config.true_peak_oversample);

  // The re-gated solve the chain and the stereo named processor use, so every
  // entry point requests the same gain for the same input.
  const float peak_db = common::measure_true_peak_dbtp(audio, config.true_peak_oversample);
  const api::detail::LoudnessStageGain stage = api::detail::loudness_stage_gain(
      audio.data(), audio.size(), 1, audio.sample_rate(), config.target_lufs, config.ceiling_db,
      peak_db, config.max_limiter_gain_reduction_db);
  const float gain = db_to_linear(stage.applied_db);
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  for (auto& sample : samples) {
    sample *= gain;
  }

  // Bound inter-sample peaks to the ceiling with a real oversampling true-peak
  // limiter instead of a per-sample clamp, so reconstructed (D/A) peaks stay at
  // or below config.ceiling_db rather than only the discrete sample peaks. The
  // limiter has look-ahead latency, so the shared runner streams trailing
  // silence and removes the delayed prefix. Its fixed-size blocks also keep
  // the limiter's oversampled scratch allocation independent of track length.
  const TruePeakLimiterConfig limiter_config = loudness_limiter_config(
      config.ceiling_db, config.true_peak_oversample, release_ms, config.apply_gain_at_input_rate);
  TruePeakLimiter limiter(limiter_config);
  api::internal::run_processor_mono(limiter, samples, audio.sample_rate());

  LoudnessOptimizeResult result;
  result.audio = Audio::from_vector(std::move(samples), audio.sample_rate());
  result.input_lufs = stage.measured_lufs;
  result.output_lufs = common::measure_lufs(result.audio);
  result.applied_gain_db = linear_to_db(gain);
  result.non_finite_substitution_count = limiter.non_finite_substitution_count();
  result.loudness_target_limited = api::detail::loudness_target_was_limited(
      stage.requested_db, stage.applied_db, config.target_lufs, result.output_lufs);
  result.latency_samples = limiter.latency_samples();
  return result;
}

}  // namespace sonare::mastering::maximizer
