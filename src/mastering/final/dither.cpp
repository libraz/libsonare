#include "mastering/final/dither.h"

#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::final {

Audio dither(const Audio& audio, const DitherConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (config.target_bits < 2 || config.target_bits > 32) {
    throw std::invalid_argument("target_bits must be in [2, 32]");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  if (config.type == DitherType::None)
    return Audio::from_vector(std::move(samples), audio.sample_rate());

  const float lsb = 1.0f / static_cast<float>(int64_t{1} << (config.target_bits - 1));
  std::mt19937 rng(config.seed);
  std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
  float error = 0.0f;
  for (auto& sample : samples) {
    float noise = dist(rng) * lsb;
    if (config.type == DitherType::Tpdf || config.type == DitherType::NoiseShaped) {
      noise += dist(rng) * lsb;
    }
    if (config.type == DitherType::NoiseShaped) {
      sample += error * 0.5f;
    }
    const float before = sample;
    sample += noise;
    if (config.type == DitherType::NoiseShaped) {
      error = before - sample;
    }
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::final
