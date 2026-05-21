#include "mastering/repair/dereverb_classical.h"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::repair {

Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.threshold >= 0.0f) || !(config.attenuation >= 0.0f && config.attenuation <= 1.0f)) {
    throw std::invalid_argument("invalid dereverb configuration");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  bool in_tail = false;
  for (auto& sample : samples) {
    if (std::abs(sample) >= config.threshold) {
      in_tail = false;
    } else if (in_tail || std::abs(sample) > 0.0f) {
      in_tail = true;
      sample *= config.attenuation;
    }
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
