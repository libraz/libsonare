#include "mastering/repair/decrackle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::repair {

Audio decrackle(const Audio& audio, const DecrackleConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.threshold > 0.0f)) throw std::invalid_argument("threshold must be positive");
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  auto output = samples;
  for (size_t i = 1; i + 1 < samples.size(); ++i) {
    std::array<float, 3> window = {samples[i - 1], samples[i], samples[i + 1]};
    std::sort(window.begin(), window.end());
    const float median = window[1];
    if (std::abs(samples[i] - median) > config.threshold) output[i] = median;
  }
  return Audio::from_vector(std::move(output), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
