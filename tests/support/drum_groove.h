#pragma once

/// @file drum_groove.h
/// @brief Synthetic drum groove: kick on beats 1 and 3, snare on 2 and 4, closed hats on 8ths.

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"

namespace sonare::test {

/// @brief Builds @p duration seconds of the groove at @p bpm and @p sr; deterministic noise.
inline Audio make_eighth_hat_groove(float bpm, int sr, float duration = 8.0f) {
  const size_t n_samples = static_cast<size_t>(duration * static_cast<float>(sr));
  std::vector<float> samples(n_samples, 0.0f);
  uint32_t state = 12345u;
  const auto noise = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
  };
  const double eighth_sec = 30.0 / bpm;
  for (int k = 0; k * eighth_sec < duration; ++k) {
    const size_t start = static_cast<size_t>(k * eighth_sec * sr);
    const int position = k % 8;
    float previous = 0.0f;
    for (size_t i = 0; i < static_cast<size_t>(0.03 * sr) && start + i < n_samples; ++i) {
      const float x = noise();
      samples[start + i] +=
          0.25f * (x - previous) * std::exp(-static_cast<float>(i) / (0.008f * sr));
      previous = x;
    }
    if (position == 0 || position == 4) {
      double phase = 0.0;
      for (size_t i = 0; i < static_cast<size_t>(0.25 * sr) && start + i < n_samples; ++i) {
        const double t = static_cast<double>(i) / sr;
        phase += constants::kTwoPiD * (50.0 + 100.0 * std::exp(-t / 0.03)) / sr;
        samples[start + i] += static_cast<float>(0.8 * std::sin(phase) * std::exp(-t / 0.08));
      }
    }
    if (position == 2 || position == 6) {
      for (size_t i = 0; i < static_cast<size_t>(0.15 * sr) && start + i < n_samples; ++i) {
        const double t = static_cast<double>(i) / sr;
        const double body = 0.4 * noise() + 0.3 * std::sin(constants::kTwoPiD * 190.0 * t);
        samples[start + i] += static_cast<float>(body * std::exp(-t / 0.05));
      }
    }
  }
  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace sonare::test
