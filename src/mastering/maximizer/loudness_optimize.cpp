#include "mastering/maximizer/loudness_optimize.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "mastering/api/internal_processor_runner.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::maximizer {

LoudnessOptimizeResult loudness_optimize(const Audio& audio, const LoudnessOptimizeConfig& config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  if (!std::isfinite(config.target_lufs) || !std::isfinite(config.ceiling_db) ||
      !std::isfinite(config.release_ms) || config.release_ms <= 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "target_lufs, ceiling_db, and release_ms must be finite and release_ms "
                          "must be positive");
  }
  if (config.true_peak_oversample != 1 && config.true_peak_oversample != 2 &&
      config.true_peak_oversample != 4 && config.true_peak_oversample != 8 &&
      config.true_peak_oversample != 16) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "oversample must be one of 1, 2, 4, 8, or 16");
  }

  const float input_lufs = common::measure_lufs(audio);
  float gain_db = std::isfinite(input_lufs) ? config.target_lufs - input_lufs : 0.0f;
  const float peak_db = common::measure_true_peak_dbtp(audio, config.true_peak_oversample);
  if (std::isfinite(peak_db)) {
    // Headroom toward the ceiling estimated from the true (inter-sample) peak so
    // the static gain alone rarely exceeds the ceiling; the limiter below catches
    // the residual inter-sample overshoots that a sample-peak clamp would miss.
    gain_db = std::min(gain_db, config.ceiling_db - peak_db);
  }

  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  const float gain = db_to_linear(gain_db);
  for (auto& sample : samples) {
    sample *= gain;
  }

  // Bound inter-sample peaks to the ceiling with a real oversampling true-peak
  // limiter instead of a per-sample clamp, so reconstructed (D/A) peaks stay at
  // or below config.ceiling_db rather than only the discrete sample peaks. The
  // limiter has look-ahead latency, so the shared runner streams trailing
  // silence and removes the delayed prefix. Its fixed-size blocks also keep
  // the limiter's oversampled scratch allocation independent of track length.
  const TruePeakLimiterConfig limiter_config =
      loudness_limiter_config(config.ceiling_db, config.true_peak_oversample, config.release_ms,
                              config.apply_gain_at_input_rate);
  TruePeakLimiter limiter(limiter_config);
  api::internal::run_processor_mono(limiter, samples, audio.sample_rate());

  LoudnessOptimizeResult result;
  result.audio = Audio::from_vector(std::move(samples), audio.sample_rate());
  result.input_lufs = input_lufs;
  result.output_lufs = common::measure_lufs(result.audio);
  result.applied_gain_db = linear_to_db(gain);
  // The returned audio is time-aligned: the limiter's look-ahead latency was
  // streamed and dropped above, so no downstream compensation is
  // needed. Report zero rather than the internal limiter latency, which would
  // otherwise make a caller double-compensate an already-aligned buffer.
  result.latency_samples = 0;
  return result;
}

}  // namespace sonare::mastering::maximizer
