/// @file sonare_c_test_helpers.h
/// @brief Shared helpers for C API tests.

#pragma once

#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "util/constants.h"

namespace {

// Generate sine wave
[[maybe_unused]] std::vector<float> generate_sine(float freq, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    samples[i] =
        std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * i / sample_rate);
  }
  return samples;
}

#ifdef SONARE_WITH_MASTERING
[[maybe_unused]] float max_abs(const float* samples, size_t length) {
  float peak = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    peak = std::max(peak, std::abs(samples[i]));
  }
  return peak;
}

[[maybe_unused]] float* non_null_sentinel_float_ptr() {
  return reinterpret_cast<float*>(static_cast<std::uintptr_t>(0x1));
}

[[maybe_unused]] std::vector<std::string> split_lines(const char* text) {
  std::vector<std::string> lines;
  std::stringstream stream(text ? text : "");
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}
#endif

// Generate click track
[[maybe_unused]] std::vector<float> generate_clicks(float bpm, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float samples_per_beat = (sample_rate * 60.0f) / bpm;
  int n_beats = static_cast<int>(duration * bpm / 60.0f);

  for (int beat = 0; beat < n_beats; ++beat) {
    size_t start = static_cast<size_t>(beat * samples_per_beat);
    size_t click_length = static_cast<size_t>(sample_rate * 0.01f);
    for (size_t i = 0; i < click_length && start + i < n_samples; ++i) {
      samples[start + i] = std::sin(static_cast<float>(sonare::constants::kPiD) * i / click_length);
    }
  }
  return samples;
}

[[maybe_unused]] std::vector<float> generate_chord(const std::vector<float>& freqs, int sample_rate,
                                                   float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples, 0.0f);
  float gain = 0.8f / static_cast<float>(freqs.size());
  for (size_t i = 0; i < n_samples; ++i) {
    for (float freq : freqs) {
      samples[i] += gain * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * i /
                                    sample_rate);
    }
  }
  return samples;
}

[[maybe_unused]] std::vector<float> generate_harmonic_chord(const std::vector<float>& freqs,
                                                            int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples, 0.0f);
  for (size_t i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    for (float freq : freqs) {
      samples[i] += 0.5f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * t);
      samples[i] += 0.25f * std::sin(4.0f * static_cast<float>(sonare::constants::kPiD) * freq * t);
      samples[i] +=
          0.125f * std::sin(6.0f * static_cast<float>(sonare::constants::kPiD) * freq * t);
    }
  }
  float peak = 0.0f;
  for (float sample : samples) {
    peak = std::max(peak, std::abs(sample));
  }
  if (peak > 0.0f) {
    for (float& sample : samples) {
      sample /= peak;
    }
  }
  return samples;
}

}  // namespace
