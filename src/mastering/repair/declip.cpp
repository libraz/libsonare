#include "mastering/repair/declip.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::repair {

Audio declip(const Audio& audio, const DeclipConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.clip_threshold > 0.0f && config.clip_threshold <= 1.0f)) {
    throw std::invalid_argument("invalid declip threshold");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  size_t i = 0;
  while (i < samples.size()) {
    if (std::abs(samples[i]) < config.clip_threshold) {
      ++i;
      continue;
    }

    const size_t start = i;
    while (i < samples.size() && std::abs(samples[i]) >= config.clip_threshold) ++i;
    const size_t end = i;
    const float left =
        start > 0 ? samples[start - 1] : (end < samples.size() ? samples[end] : 0.0f);
    const float right = end < samples.size() ? samples[end] : left;
    const size_t length = end - start;
    for (size_t j = start; j < end; ++j) {
      const float t = static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
      samples[j] =
          std::clamp(left + (right - left) * t, -config.clip_threshold, config.clip_threshold);
    }
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
