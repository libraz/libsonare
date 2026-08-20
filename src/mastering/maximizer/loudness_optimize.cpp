#include "mastering/maximizer/loudness_optimize.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "mastering/api/audio_utils.h"
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
  if (!std::isfinite(config.max_limiter_gain_reduction_db) ||
      config.max_limiter_gain_reduction_db < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "max_limiter_gain_reduction_db must be finite and >= 0");
  }
  if (config.true_peak_oversample != 1 && config.true_peak_oversample != 2 &&
      config.true_peak_oversample != 4 && config.true_peak_oversample != 8 &&
      config.true_peak_oversample != 16) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "oversample must be one of 1, 2, 4, 8, or 16");
  }

  const float input_lufs = common::measure_lufs(audio);
  const float requested_gain_db =
      std::isfinite(input_lufs) ? config.target_lufs - input_lufs : 0.0f;
  float gain_db = requested_gain_db;
  const float peak_db = common::measure_true_peak_dbtp(audio, config.true_peak_oversample);
  if (std::isfinite(peak_db)) {
    // Headroom toward the ceiling estimated from the true (inter-sample) peak,
    // plus the depth the limiter below is allowed to be driven to. Clamping at
    // the bare headroom instead would leave a peak-normalized input (headroom
    // ~0 dB) at its input loudness whatever target was asked for, and give the
    // limiter nothing to do; the limiter is what makes the target reachable.
    gain_db = std::min(gain_db, (config.ceiling_db - peak_db) +
                                    std::max(config.max_limiter_gain_reduction_db, 0.0f));
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
  result.loudness_target_limited =
      std::isfinite(input_lufs) &&
      api::detail::loudness_target_was_limited(requested_gain_db, gain_db, config.target_lufs,
                                               result.output_lufs);
  // The returned audio is time-aligned: the limiter's look-ahead latency was
  // streamed and dropped above, so no downstream compensation is
  // needed. Report zero rather than the internal limiter latency, which would
  // otherwise make a caller double-compensate an already-aligned buffer.
  result.latency_samples = 0;
  return result;
}

}  // namespace sonare::mastering::maximizer
