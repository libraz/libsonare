/// @file stream_analyzer_test_helpers.h
/// @brief Shared helpers for StreamAnalyzer tests.

#pragma once

#include <algorithm>
#include <array>
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

/// Chord bed with a periodic click: a triad of sine partials that changes every
/// `chord_period_sec`, plus a short decaying click on every beat. Bar tracking
/// only starts once the BPM estimate is confident, and bars are only recorded
/// while per-frame chord detection is confident, so a fixture that drives the
/// bar path needs both an unambiguous onset period and real chord content.
[[maybe_unused]] std::vector<float> generate_chord_click_bed(int total_samples, int sr, float bpm,
                                                             float chord_period_sec = 2.0f) {
  std::vector<float> audio(static_cast<size_t>(std::max(total_samples, 0)), 0.0f);
  const std::array<std::array<float, 3>, 2> chords = {
      std::array<float, 3>{261.63f, 329.63f, 392.0f},  // C major
      std::array<float, 3>{392.0f, 493.88f, 587.33f},  // G major
  };
  const float beat_samples = 60.0f * static_cast<float>(sr) / bpm;
  const size_t chord_samples =
      static_cast<size_t>(std::max(1.0f, chord_period_sec * static_cast<float>(sr)));
  for (size_t i = 0; i < audio.size(); ++i) {
    const auto& chord = chords[(i / chord_samples) % chords.size()];
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    for (float frequency : chord) {
      audio[i] += 0.15f * std::sin(kTwoPi * frequency * t);
    }
    const size_t beat_start = static_cast<size_t>(
        static_cast<float>(static_cast<size_t>(i / beat_samples)) * beat_samples);
    const size_t into_beat = i - beat_start;
    if (into_beat < 8) {
      audio[i] += 0.8f * (1.0f - static_cast<float>(into_beat) / 8.0f);
    }
  }
  return audio;
}

}  // namespace
