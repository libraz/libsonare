#include "mastering/repair/declip.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::repair {
namespace {

float cubic_hermite(float y0, float y1, float y2, float y3, float t) {
  const float m1 = 0.5f * (y2 - y0);
  const float m2 = 0.5f * (y3 - y1);
  const float t2 = t * t;
  const float t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * y1 + (t3 - 2.0f * t2 + t) * m1 +
         (-2.0f * t3 + 3.0f * t2) * y2 + (t3 - t2) * m2;
}

bool has_cubic_context(size_t start, size_t end, size_t size) {
  return start >= 2 && end + 1 < size;
}

}  // namespace

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
    const bool use_cubic = has_cubic_context(start, end, samples.size());
    for (size_t j = start; j < end; ++j) {
      const float t = static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
      const float reconstructed =
          use_cubic ? cubic_hermite(samples[start - 2], left, right, samples[end + 1], t)
                    : left + (right - left) * t;
      samples[j] = std::clamp(reconstructed, -config.clip_threshold, config.clip_threshold);
    }
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
