#include "mastering/maximizer/loudness_optimize.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "mastering/common/loudness_measure.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::maximizer {

LoudnessOptimizeResult loudness_optimize(const Audio& audio, const LoudnessOptimizeConfig& config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  if (config.true_peak_oversample < 1)
    throw SonareException(ErrorCode::InvalidParameter, "oversample must be positive");

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
  // limiter has look-ahead latency, so pad by its latency, process, and drop the
  // leading delayed samples to keep the output time-aligned (mirrors the chain's
  // run_processor_mono helper).
  const int num_samples = static_cast<int>(samples.size());
  TruePeakLimiterConfig limiter_config;
  limiter_config.ceiling_db = config.ceiling_db;
  limiter_config.oversample_factor = config.true_peak_oversample;
  TruePeakLimiter limiter(limiter_config);
  limiter.prepare(static_cast<double>(audio.sample_rate()), num_samples);
  const int latency = limiter.latency_samples();
  if (latency > 0) {
    limiter.reset();
    limiter.prepare(static_cast<double>(audio.sample_rate()), num_samples + latency);
    samples.resize(static_cast<std::size_t>(num_samples) + static_cast<std::size_t>(latency), 0.0f);
    float* channel_ptrs[] = {samples.data()};
    limiter.process(channel_ptrs, 1, num_samples + latency);
    samples.erase(samples.begin(), samples.begin() + latency);
    samples.resize(static_cast<std::size_t>(num_samples));
  } else {
    float* channel_ptrs[] = {samples.data()};
    limiter.process(channel_ptrs, 1, num_samples);
  }

  LoudnessOptimizeResult result;
  result.audio = Audio::from_vector(std::move(samples), audio.sample_rate());
  result.input_lufs = input_lufs;
  result.output_lufs = common::measure_lufs(result.audio);
  result.applied_gain_db = linear_to_db(gain);
  result.latency_samples = latency;
  return result;
}

}  // namespace sonare::mastering::maximizer
