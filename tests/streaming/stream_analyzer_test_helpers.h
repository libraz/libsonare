/// @file stream_analyzer_test_helpers.h
/// @brief Shared helpers for StreamAnalyzer tests.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "streaming/stream_analyzer.h"
#include "streaming/stream_analyzer_utils.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using sonare::constants::kTwoPi;

[[maybe_unused]] std::vector<float> generate_sine(int samples, float freq, int sr) {
  std::vector<float> result(samples);
  for (int i = 0; i < samples; ++i) {
    result[i] = std::sin(kTwoPi * freq * i / sr);
  }
  return result;
}

/// Generate a click train at a known tempo: a short decaying 1 kHz sine burst
/// at each beat. Mirrors the rhythmic synthetic signal used by the existing
/// BPM tests so onset detection has clear, periodic energy spikes.
[[maybe_unused]] std::vector<float> generate_click_train(int total_samples, float bpm, int sr) {
  std::vector<float> audio(static_cast<size_t>(std::max(total_samples, 0)), 0.0f);
  const float beat_interval_samples = 60.0f * static_cast<float>(sr) / bpm;
  const int n_beats = static_cast<int>(total_samples / beat_interval_samples);
  for (int beat = 0; beat <= n_beats; ++beat) {
    const int beat_start = static_cast<int>(beat * beat_interval_samples);
    const int click_len = std::min(220, total_samples - beat_start);  // ~10ms
    for (int i = 0; i < click_len; ++i) {
      const int idx = beat_start + i;
      if (idx >= 0 && idx < total_samples) {
        const float decay = std::exp(-static_cast<float>(i) / 50.0f);
        audio[static_cast<size_t>(idx)] = decay * std::sin(kTwoPi * 1000.0f * i / sr);
      }
    }
  }
  return audio;
}

}  // namespace
