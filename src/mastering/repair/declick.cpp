#include "mastering/repair/declick.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::repair {
Audio declick(const Audio& audio, const DeclickConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.threshold > 0.0f) || !(config.neighbor_ratio > 0.0f) ||
      config.max_click_samples == 0) {
    throw std::invalid_argument("invalid declick configuration");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  std::vector<float> output = samples;
  size_t i = 1;
  while (i + 1 < samples.size()) {
    if (std::abs(samples[i]) < config.threshold) {
      ++i;
      continue;
    }

    const size_t start = i;
    float peak = 0.0f;
    while (i + 1 < samples.size() && std::abs(samples[i]) >= config.threshold) {
      peak = std::max(peak, std::abs(samples[i]));
      ++i;
    }
    const size_t end = i;
    const size_t length = end - start;
    const float local = std::max({std::abs(samples[start - 1]), std::abs(samples[end]), 1e-6f});
    if (length <= config.max_click_samples && peak > local * config.neighbor_ratio) {
      const float left = output[start - 1];
      const float right = samples[end];
      for (size_t j = start; j < end; ++j) {
        const float t = static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
        output[j] = left + (right - left) * t;
      }
    }
  }
  return Audio::from_vector(std::move(output), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
