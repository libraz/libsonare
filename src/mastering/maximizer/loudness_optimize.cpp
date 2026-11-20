#include "mastering/maximizer/loudness_optimize.h"

#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "mastering/api/audio_utils.h"
#include "mastering/api/internal_processor_runner.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/parameter_checks.h"

namespace sonare::mastering::maximizer {

void validate_loudness_params(float target_lufs, float ceiling_dbtp, float release_ms,
                              float max_limiter_gain_reduction_db, int true_peak_oversample) {
  util::check::finite_float(target_lufs, "target_lufs");
  util::check::at_most(ceiling_dbtp, 0.0, "ceiling_dbtp");
  util::check::greater_than(release_ms, 0.0, "release_ms");
  util::check::at_least(max_limiter_gain_reduction_db, 0.0, "max_limiter_gain_reduction_db");
  util::check::one_of(true_peak_oversample, std::array<int, 5>{1, 2, 4, 8, 16},
                      "true_peak_oversample");
}

LoudnessOptimizeResult loudness_optimize(const Audio& audio, const LoudnessOptimizeConfig& config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  validate_loudness_params(config.target_lufs, config.ceiling_dbtp, config.release_ms,
                           config.max_limiter_gain_reduction_db, config.true_peak_oversample);

  // The re-gated solve the chain and the stereo named processor use, so every
  // entry point requests the same gain for the same input.
  const float peak_db = common::measure_true_peak_dbtp(audio, config.true_peak_oversample);
  const api::detail::LoudnessStageGain stage = api::detail::loudness_stage_gain(
      audio.data(), audio.size(), 1, audio.sample_rate(), config.target_lufs, config.ceiling_dbtp,
      peak_db, config.max_limiter_gain_reduction_db);
  const float gain = db_to_linear(stage.applied_db);
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  for (auto& sample : samples) {
    sample *= gain;
  }

  // Bound inter-sample peaks to the ceiling with a real oversampling true-peak
  // limiter instead of a per-sample clamp, so reconstructed (D/A) peaks stay at
  // or below config.ceiling_dbtp rather than only the discrete sample peaks. The
  // limiter has look-ahead latency, so the shared runner streams trailing
  // silence and removes the delayed prefix. Its fixed-size blocks also keep
  // the limiter's oversampled scratch allocation independent of track length.
  const TruePeakLimiterConfig limiter_config =
      loudness_limiter_config(config.ceiling_dbtp, config.true_peak_oversample, config.release_ms,
                              config.apply_gain_at_input_rate);
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
