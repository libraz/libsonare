#include "mastering/repair/dehum.h"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sonare::mastering::repair {
namespace {

struct Notch {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

Notch make_notch(float frequency_hz, float sample_rate, float q) {
  const float omega = 2.0f * 3.14159265358979323846f * frequency_hz / sample_rate;
  const float alpha = std::sin(omega) / (2.0f * q);
  const float cosw = std::cos(omega);
  const float a0 = 1.0f + alpha;
  return {1.0f / a0, -2.0f * cosw / a0, 1.0f / a0, -2.0f * cosw / a0, (1.0f - alpha) / a0};
}

}  // namespace

Audio dehum(const Audio& audio, const DehumConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.fundamental_hz > 0.0f) || config.harmonics < 1 || !(config.q > 0.0f)) {
    throw std::invalid_argument("invalid dehum configuration");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
    const float frequency = config.fundamental_hz * static_cast<float>(harmonic);
    if (frequency >= static_cast<float>(audio.sample_rate()) * 0.5f) break;
    auto notch = make_notch(frequency, static_cast<float>(audio.sample_rate()), config.q);
    for (auto& sample : samples) sample = notch.process(sample);
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
