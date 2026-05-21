#include "mastering/maximizer/loudness_optimize.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "analysis/meter/lufs.h"
#include "analysis/meter/true_peak.h"

namespace sonare::mastering::maximizer {

namespace {

float db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }

float linear_to_db(float value) { return value <= 0.0f ? -120.0f : 20.0f * std::log10(value); }

}  // namespace

LoudnessOptimizeResult loudness_optimize(const Audio& audio, const LoudnessOptimizeConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (config.true_peak_oversample < 1) throw std::invalid_argument("oversample must be positive");

  const auto input_loudness = analysis::meter::lufs(audio);
  float gain_db = std::isfinite(input_loudness.integrated_lufs)
                      ? config.target_lufs - input_loudness.integrated_lufs
                      : 0.0f;
  const float peak_db = analysis::meter::true_peak_db(audio, config.true_peak_oversample);
  if (std::isfinite(peak_db)) {
    gain_db = std::min(gain_db, config.ceiling_db - peak_db);
  }

  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  const float gain = db_to_linear(gain_db);
  const float ceiling = db_to_linear(config.ceiling_db);
  for (auto& sample : samples) {
    sample *= gain;
    sample = std::clamp(sample, -ceiling, ceiling);
  }

  LoudnessOptimizeResult result;
  result.audio = Audio::from_vector(std::move(samples), audio.sample_rate());
  result.input_lufs = input_loudness.integrated_lufs;
  result.output_lufs = analysis::meter::lufs(result.audio).integrated_lufs;
  result.applied_gain_db = linear_to_db(gain);
  return result;
}

}  // namespace sonare::mastering::maximizer
