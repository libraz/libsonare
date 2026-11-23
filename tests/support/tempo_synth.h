#pragma once

/// @file tempo_synth.h
/// @brief Synthetic tempo material: a noise click, a duple kick and hat groove, a 6/8 groove.

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"

namespace sonare::test {

namespace tempo_synth_detail {

/// Deterministic uniform noise in [-1, 1).
class Noise {
 public:
  float operator()() {
    state_ = state_ * 1664525u + 1013904223u;
    return static_cast<float>(state_ >> 8) / 8388608.0f - 1.0f;
  }

 private:
  uint32_t state_ = 1u;
};

inline void add_burst(std::vector<float>& y, int sr, double t, float amp, float tau_sec,
                      float length_sec, Noise& noise) {
  const size_t start = static_cast<size_t>(t * sr);
  const size_t length = static_cast<size_t>(length_sec * sr);
  if (start + length > y.size()) return;
  for (size_t i = 0; i < length; ++i) {
    y[start + i] += amp * noise() * std::exp(-static_cast<float>(i) / (tau_sec * sr));
  }
}

inline void add_kick(std::vector<float>& y, int sr, double t) {
  const size_t start = static_cast<size_t>(t * sr);
  const size_t length = static_cast<size_t>(0.15 * sr);
  if (start + length > y.size()) return;
  for (size_t i = 0; i < length; ++i) {
    const double k = static_cast<double>(i) / sr;
    const double phase =
        constants::kTwoPiD * (50.0 * k + 40.0 * (1.0 - std::exp(-k * 30.0)) / 30.0);
    y[start + i] += static_cast<float>(0.9 * std::sin(phase) * std::exp(-k * 25.0));
  }
}

}  // namespace tempo_synth_detail

/// @brief A short decaying noise burst on every beat; no subdivision.
inline Audio make_noise_click(float bpm, int sr, float duration) {
  std::vector<float> y(static_cast<size_t>(duration * static_cast<float>(sr)), 0.0f);
  tempo_synth_detail::Noise noise;
  const double beat = 60.0 / bpm;
  for (double t = 0.0; t < duration; t += beat) {
    tempo_synth_detail::add_burst(y, sr, t, 0.8f, 0.008f, 0.05f, noise);
  }
  return Audio::from_vector(std::move(y), sr);
}

/// @brief Kick on every beat, snare on 2 and 4, hats on the eighths.
inline Audio make_kick_hat_groove(float bpm, int sr, float duration) {
  std::vector<float> y(static_cast<size_t>(duration * static_cast<float>(sr)), 0.0f);
  tempo_synth_detail::Noise noise;
  const double beat = 60.0 / bpm;
  int n = 0;
  for (double t = 0.0; t < duration; t += beat, ++n) {
    tempo_synth_detail::add_kick(y, sr, t);
    if (n % 2 == 1) tempo_synth_detail::add_burst(y, sr, t, 0.6f, 0.03f, 0.12f, noise);
    tempo_synth_detail::add_burst(y, sr, t, 0.2f, 0.004f, 0.05f, noise);
    tempo_synth_detail::add_burst(y, sr, t + beat / 2.0, 0.15f, 0.004f, 0.05f, noise);
  }
  return Audio::from_vector(std::move(y), sr);
}

/// @brief 6/8: a kick on each dotted quarter at @p dotted_quarter_bpm, hats on the other eighths.
inline Audio make_compound_groove(float dotted_quarter_bpm, int sr, float duration,
                                  float hat = 0.2f) {
  std::vector<float> y(static_cast<size_t>(duration * static_cast<float>(sr)), 0.0f);
  tempo_synth_detail::Noise noise;
  const double beat = 60.0 / dotted_quarter_bpm;
  for (double t = 0.0; t < duration; t += beat) {
    tempo_synth_detail::add_kick(y, sr, t);
    for (int j = 1; j < 3; ++j) {
      tempo_synth_detail::add_burst(y, sr, t + j * beat / 3.0, hat, 0.004f, 0.05f, noise);
    }
  }
  return Audio::from_vector(std::move(y), sr);
}

}  // namespace sonare::test
