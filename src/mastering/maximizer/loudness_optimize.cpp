#include "mastering/maximizer/loudness_optimize.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "analysis/meter/lufs.h"
#include "analysis/meter/true_peak.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "util/db.h"

namespace sonare::mastering::maximizer {

LoudnessOptimizeResult loudness_optimize(const Audio& audio, const LoudnessOptimizeConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (config.true_peak_oversample < 1) throw std::invalid_argument("oversample must be positive");

  const auto input_loudness = analysis::meter::lufs(audio);
  float gain_db = std::isfinite(input_loudness.integrated_lufs)
                      ? config.target_lufs - input_loudness.integrated_lufs
                      : 0.0f;
  const float peak_db = analysis::meter::true_peak_db(audio, config.true_peak_oversample);
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
  result.input_lufs = input_loudness.integrated_lufs;
  result.output_lufs = analysis::meter::lufs(result.audio).integrated_lufs;
  result.applied_gain_db = linear_to_db(gain);
  return result;
}

}  // namespace sonare::mastering::maximizer
