#pragma once

/// @file rate_material.h
/// @brief Rate-invariance test material, synthesized at 22050 Hz then resampled.

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "core/resample.h"
#include "util/constants.h"

namespace sonare::test {

enum class RateMaterial {
  TriadTurnaround,  ///< C, Am, F, G triads, 2 s each, 0.5 s rests
  Cadence,          ///< F, G7, C, 1.5 s each
  Clicks            ///< 120 BPM, 1 kHz bursts from 0.25 s, 8.25 s
};

namespace rate_material_detail {

constexpr int kBaseRate = constants::kDefaultSampleRate;

inline double midi_hz(int midi) {
  return constants::kA4Hz *
         std::pow(2.0, (midi - constants::kMidiA4) / constants::kSemitonesPerOctave);
}

/// Adds one chord of 3 harmonics (1, 0.5, 0.25), 10 ms linear attack, 20 ms release.
inline void add_chord(std::vector<float>& samples, const std::vector<int>& midis, double on,
                      double len) {
  constexpr double attack = 0.010, release = 0.020;
  const size_t first = static_cast<size_t>(on * kBaseRate);
  const size_t last =
      std::min(samples.size(), static_cast<size_t>((on + len + release) * kBaseRate));
  for (int midi : midis) {
    const double f = midi_hz(midi);
    for (size_t i = first; i < last; ++i) {
      const double t = static_cast<double>(i) / kBaseRate - on;
      double env = t < attack ? t / attack : 1.0;
      if (t > len) env *= std::max(0.0, 1.0 - (t - len) / release);
      double v = 0.0;
      for (int h = 1; h <= 3; ++h) {
        v += std::pow(0.5, h - 1) * std::sin(constants::kTwoPiD * f * h * t);
      }
      samples[i] += static_cast<float>(0.1 * env * v);
    }
  }
}

}  // namespace rate_material_detail

/// @brief Builds @p m at @p sr. All content is below 10 kHz; no randomness.
/// @details Voicings (MIDI): C 60 64 67, Am 57 60 64, F 53 57 60, G 55 59 62 (turnaround);
///          F 53 57 60, G7 55 59 62 65, C 48 55 60 64 (cadence).
inline Audio make_rate_material(RateMaterial m, int sr) {
  using namespace rate_material_detail;
  std::vector<float> samples;
  switch (m) {
    case RateMaterial::TriadTurnaround: {
      const std::vector<std::vector<int>> voicings = {
          {60, 64, 67}, {57, 60, 64}, {53, 57, 60}, {55, 59, 62}};
      samples.assign(static_cast<size_t>(kBaseRate * 2.0 * voicings.size()), 0.0f);
      for (size_t b = 0; b < voicings.size(); ++b) {
        const double len = b + 1 == voicings.size() ? 2.0 : 1.5;
        add_chord(samples, voicings[b], 2.0 * static_cast<double>(b), len);
      }
      break;
    }
    case RateMaterial::Cadence: {
      const std::vector<std::vector<int>> voicings = {
          {53, 57, 60}, {55, 59, 62, 65}, {48, 55, 60, 64}};
      samples.assign(static_cast<size_t>(kBaseRate * 1.5 * voicings.size()), 0.0f);
      for (size_t b = 0; b < voicings.size(); ++b) {
        add_chord(samples, voicings[b], 1.5 * static_cast<double>(b), 1.5);
      }
      break;
    }
    case RateMaterial::Clicks: {
      samples.assign(static_cast<size_t>(kBaseRate * 8.25), 0.0f);
      const size_t burst = static_cast<size_t>(0.010 * kBaseRate);
      const size_t period = static_cast<size_t>(0.5 * kBaseRate);
      for (size_t start = period / 2; start + burst <= samples.size(); start += period) {
        for (size_t i = 0; i < burst; ++i) {
          const double hann =
              0.5 - 0.5 * std::cos(constants::kTwoPiD * static_cast<double>(i) / burst);
          samples[start + i] += static_cast<float>(
              0.8 * hann * std::sin(constants::kTwoPiD * 1000.0 * i / kBaseRate));
        }
      }
      break;
    }
  }
  Audio audio = Audio::from_vector(std::move(samples), kBaseRate);
  return sr == kBaseRate ? audio : resample(audio, sr);
}

}  // namespace sonare::test
