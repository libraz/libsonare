#include "mastering/repair/trim_silence.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace sonare::mastering::repair {

Audio trim_silence(const Audio& audio, const TrimSilenceConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.threshold >= 0.0f)) throw std::invalid_argument("threshold must be non-negative");

  size_t first = 0;
  while (first < audio.size() && std::abs(audio[first]) <= config.threshold) ++first;
  if (first == audio.size()) return Audio::from_vector({}, audio.sample_rate());

  size_t last = audio.size() - 1;
  while (last > first && std::abs(audio[last]) <= config.threshold) --last;
  first = first > config.padding_samples ? first - config.padding_samples : 0;
  last = std::min(audio.size() - 1, last + config.padding_samples);
  return audio.slice_samples(first, last + 1);
}

}  // namespace sonare::mastering::repair
