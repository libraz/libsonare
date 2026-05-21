#include "mastering/repair/denoise_classical.h"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::repair {

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.noise_floor >= 0.0f) ||
      !(config.attenuation >= 0.0f && config.attenuation <= 1.0f)) {
    throw std::invalid_argument("invalid denoise configuration");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  for (auto& sample : samples) {
    const float level = std::abs(sample);
    if (level <= config.noise_floor) {
      sample *= config.attenuation;
    } else if (config.noise_floor > 0.0f && level < config.noise_floor * 2.0f) {
      const float transition = (level - config.noise_floor) / config.noise_floor;
      const float gain = config.attenuation + (1.0f - config.attenuation) * transition;
      sample *= gain;
    }
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
