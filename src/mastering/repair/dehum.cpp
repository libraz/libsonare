#include "mastering/repair/dehum.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#include "rt/biquad_design.h"
#include "util/constants.h"

namespace sonare::mastering::repair {

using sonare::constants::kTwoPi;
using sonare::constants::kTwoPiD;

namespace {

struct Notch {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  void set_coefficients(float frequency_hz, float sample_rate, float q) {
    const float omega = kTwoPi * frequency_hz / sample_rate;
    const auto coeffs = rt::rbj_notch(omega, q);
    b0 = coeffs.b0;
    b1 = coeffs.b1;
    b2 = coeffs.b2;
    a1 = coeffs.a1;
    a2 = coeffs.a2;
  }

  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

Notch make_notch(float frequency_hz, float sample_rate, float q) {
  Notch notch;
  notch.set_coefficients(frequency_hz, sample_rate, q);
  return notch;
}

struct PllTracker {
  float frequency_hz = 50.0f;
  float phase = 0.0f;

  float process(float sample, float target_hz, int sample_rate, const DehumConfig& config) {
    const float detector = sample * std::cos(phase);
    const float pull = config.adaptation * 0.001f * (target_hz - frequency_hz);
    frequency_hz += config.pll_bandwidth * detector + pull;
    frequency_hz =
        std::clamp(frequency_hz, std::max(1.0f, config.fundamental_hz - config.search_range_hz),
                   std::min(static_cast<float>(sample_rate) * 0.49f,
                            config.fundamental_hz + config.search_range_hz));
    phase += kTwoPi * frequency_hz / static_cast<float>(sample_rate);
    if (phase > kTwoPi) {
      phase -= kTwoPi;
    }
    return frequency_hz;
  }
};

double projected_energy(const std::vector<float>& samples, size_t begin, size_t end,
                        float frequency_hz, int sample_rate) {
  double sin_sum = 0.0;
  double cos_sum = 0.0;
  for (size_t i = begin; i < end; ++i) {
    const double phase =
        kTwoPiD * frequency_hz * static_cast<double>(i) / static_cast<double>(sample_rate);
    sin_sum += samples[i] * std::sin(phase);
    cos_sum += samples[i] * std::cos(phase);
  }
  return sin_sum * sin_sum + cos_sum * cos_sum;
}

float estimate_fundamental(const std::vector<float>& samples, size_t begin, size_t end,
                           int sample_rate, const DehumConfig& config, float previous_hz) {
  const float low = std::max(1.0f, previous_hz - config.search_range_hz);
  const float high =
      std::min(static_cast<float>(sample_rate) * 0.49f, previous_hz + config.search_range_hz);
  float best_hz = previous_hz;
  double best_energy = -1.0;
  constexpr int kSteps = 16;
  for (int step = 0; step <= kSteps; ++step) {
    const float hz = low + (high - low) * static_cast<float>(step) / static_cast<float>(kSteps);
    const double energy = projected_energy(samples, begin, end, hz, sample_rate);
    if (energy > best_energy) {
      best_energy = energy;
      best_hz = hz;
    }
  }
  return previous_hz + config.adaptation * (best_hz - previous_hz);
}

}  // namespace

Audio dehum(const Audio& audio, const DehumConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.fundamental_hz > 0.0f) || config.harmonics < 1 || !(config.q > 0.0f) ||
      config.search_range_hz < 0.0f || config.adaptation < 0.0f || config.adaptation > 1.0f ||
      config.frame_size < 16 || config.pll_bandwidth < 0.0f) {
    throw std::invalid_argument("invalid dehum configuration");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  if (config.adaptive) {
    const int sample_rate = audio.sample_rate();
    float fundamental = config.fundamental_hz;
    float target_fundamental = fundamental;
    PllTracker tracker{fundamental, 0.0f};
    std::vector<Notch> notches(static_cast<size_t>(config.harmonics));
    for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
      notches[static_cast<size_t>(harmonic - 1)] = make_notch(
          fundamental * static_cast<float>(harmonic), static_cast<float>(sample_rate), config.q);
    }
    for (size_t begin = 0; begin < samples.size();
         begin += static_cast<size_t>(config.frame_size)) {
      const size_t end = std::min(samples.size(), begin + static_cast<size_t>(config.frame_size));
      target_fundamental =
          estimate_fundamental(samples, begin, end, sample_rate, config, target_fundamental);
      for (size_t i = begin; i < end; ++i) {
        fundamental = tracker.process(samples[i], target_fundamental, sample_rate, config);
        for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
          const float frequency = fundamental * static_cast<float>(harmonic);
          if (frequency >= static_cast<float>(sample_rate) * 0.5f) break;
          auto& notch = notches[static_cast<size_t>(harmonic - 1)];
          notch.set_coefficients(frequency, static_cast<float>(sample_rate), config.q);
          samples[i] = notch.process(samples[i]);
        }
      }
    }
    return Audio::from_vector(std::move(samples), audio.sample_rate());
  }
  for (int harmonic = 1; harmonic <= config.harmonics; ++harmonic) {
    const float frequency = config.fundamental_hz * static_cast<float>(harmonic);
    if (frequency >= static_cast<float>(audio.sample_rate()) * 0.5f) break;
    auto notch = make_notch(frequency, static_cast<float>(audio.sample_rate()), config.q);
    for (auto& sample : samples) sample = notch.process(sample);
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
