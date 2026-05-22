#include "mastering/repair/trim_silence.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "util/constants.h"
#include "util/dsp_primitives.h"

namespace sonare::mastering::repair {
namespace {

float sample_loudness_db(const Audio& audio, size_t center, size_t radius) {
  const size_t begin = center > radius ? center - radius : 0;
  const size_t end = std::min(audio.size(), center + radius + 1);
  const float window_rms = rms(audio.data() + begin, std::max<size_t>(1, end - begin));
  return window_rms <= 1.0e-12f ? sonare::constants::kFloorDb
                                 : static_cast<float>(20.0 * std::log10(window_rms));
}

bool is_active_sample(const Audio& audio, size_t index, const TrimSilenceConfig& config,
                      size_t loudness_radius) {
  if (config.mode == TrimSilenceMode::Peak) {
    return std::abs(audio[index]) > config.threshold;
  }
  return sample_loudness_db(audio, index, loudness_radius) > config.gate_lufs;
}

}  // namespace

Audio trim_silence(const Audio& audio, const TrimSilenceConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.threshold >= 0.0f)) throw std::invalid_argument("threshold must be non-negative");
  if (!(config.window_ms > 0.0f)) throw std::invalid_argument("window_ms must be positive");

  const size_t loudness_radius =
      std::max<size_t>(1, static_cast<size_t>(audio.sample_rate() * config.window_ms * 0.0005f));
  size_t first = 0;
  while (first < audio.size() && !is_active_sample(audio, first, config, loudness_radius)) ++first;
  if (first == audio.size()) return Audio::from_vector({}, audio.sample_rate());

  size_t last = audio.size() - 1;
  while (last > first && !is_active_sample(audio, last, config, loudness_radius)) --last;
  first = first > config.padding_samples ? first - config.padding_samples : 0;
  last = std::min(audio.size() - 1, last + config.padding_samples);
  return audio.slice_samples(first, last + 1);
}

}  // namespace sonare::mastering::repair
