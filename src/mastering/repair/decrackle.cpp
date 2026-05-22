#include "mastering/repair/decrackle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::repair {
namespace {

float soft_threshold(float value, float threshold) {
  if (value > threshold) return value - threshold;
  if (value < -threshold) return value + threshold;
  return 0.0f;
}

float median_abs(std::vector<float> values) {
  if (values.empty()) return 0.0f;
  for (auto& value : values) value = std::abs(value);
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

float variance(const std::vector<float>& values) {
  if (values.empty()) return 0.0f;
  const double mean =
      std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  double sum = 0.0;
  for (float value : values) {
    const double centered = static_cast<double>(value) - mean;
    sum += centered * centered;
  }
  return static_cast<float>(sum / static_cast<double>(values.size()));
}

float bayes_shrink_threshold(const std::vector<float>& details, float noise_sigma,
                             float max_threshold) {
  if (details.empty()) return max_threshold;
  const float detail_variance = variance(details);
  const float noise_variance = noise_sigma * noise_sigma;
  const float signal_sigma = std::sqrt(std::max(0.0f, detail_variance - noise_variance));
  if (signal_sigma <= 1e-9f) {
    return max_threshold;
  }
  return std::min(max_threshold, noise_variance / signal_sigma);
}

void haar_shrink(std::vector<float>& samples, int levels, float threshold) {
  size_t active = samples.size();
  std::vector<size_t> active_sizes;
  float noise_sigma = 0.0f;
  for (int level = 0; level < levels && active >= 2; ++level) {
    const size_t pairs = active / 2;
    std::vector<float> temp(active, 0.0f);
    std::vector<float> details(pairs, 0.0f);
    for (size_t i = 0; i < pairs; ++i) {
      const float a = samples[2 * i];
      const float b = samples[2 * i + 1];
      temp[i] = 0.5f * (a + b);
      details[i] = 0.5f * (a - b);
    }

    if (level == 0) {
      noise_sigma = median_abs(details) / 0.67448975f;
    }
    const float level_threshold = bayes_shrink_threshold(details, noise_sigma, threshold);
    for (size_t i = 0; i < pairs; ++i) {
      temp[pairs + i] = soft_threshold(details[i], level_threshold);
    }
    if (active % 2 != 0) {
      temp[active - 1] = samples[active - 1];
    }
    std::copy(temp.begin(), temp.end(), samples.begin());
    active_sizes.push_back(active);
    active = pairs;
  }

  for (int level = static_cast<int>(active_sizes.size()) - 1; level >= 0; --level) {
    const size_t reconstruct = active_sizes[static_cast<size_t>(level)];
    const size_t pairs = reconstruct / 2;
    if (pairs == 0) continue;
    std::vector<float> temp(reconstruct, 0.0f);
    for (size_t i = 0; i < pairs; ++i) {
      const float avg = samples[i];
      const float detail = samples[pairs + i];
      temp[2 * i] = avg + detail;
      temp[2 * i + 1] = avg - detail;
    }
    if (reconstruct % 2 != 0) temp[reconstruct - 1] = samples[reconstruct - 1];
    std::copy(temp.begin(), temp.end(), samples.begin());
  }
}

}  // namespace

Audio decrackle(const Audio& audio, const DecrackleConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.threshold > 0.0f) || config.levels < 1) {
    throw std::invalid_argument("invalid decrackle configuration");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  if (config.mode == DecrackleMode::WaveletShrinkage) {
    haar_shrink(samples, config.levels, config.threshold);
    return Audio::from_vector(std::move(samples), audio.sample_rate());
  }
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
