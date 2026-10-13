/// @file repair_drum_fixtures.h
/// @brief Drum loops the impulsive and hum detectors must read as programme, not as defects.
///
/// A kick is a short tonal burst near 50 Hz and a snare a burst of broadband noise; both repeat,
/// so a detector that reads the whole input at once sees a prominent low peak and many samples
/// far from their local median. Each loop is deterministic, so a count planted between the hits
/// is the detector's whole answer.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/constants.h"

namespace sonare::test::repair_drums {

constexpr int kSampleRate = 48000;
constexpr std::size_t kLoopFrames = 4 * kSampleRate;

/// A kick every half second: a sine swept from 110 Hz down to 50 Hz, 2 ms fade-in, 120 ms decay.
inline std::vector<double> kick_loop() {
  std::vector<double> out(kLoopFrames, 0.0);
  const std::size_t period = kSampleRate / 2;
  double phase = 0.0;
  for (std::size_t i = 0; i < kLoopFrames; ++i) {
    const double t = static_cast<double>(i % period) / kSampleRate;
    if (i % period == 0) phase = 0.0;
    phase += constants::kTwoPiD * (50.0 + 60.0 * std::exp(-t / 0.03)) / kSampleRate;
    out[i] = 0.8 * std::min(1.0, t / 0.002) * std::exp(-t / 0.12) * std::sin(phase);
  }
  return out;
}

/// A snare every second: a quarter second of LCG white noise at 0.9, 40 ms decay.
inline std::vector<double> snare_loop() {
  std::vector<double> out(kLoopFrames, 0.0);
  const std::size_t period = kSampleRate;
  const std::size_t length = kSampleRate / 4;
  std::uint32_t state = 12345u;
  std::vector<double> hit(length);
  for (std::size_t i = 0; i < length; ++i) {
    state = state * 1664525u + 1013904223u;
    const double noise = static_cast<double>(state) / 2147483648.0 - 1.0;
    hit[i] = 0.9 * std::exp(-static_cast<double>(i) / kSampleRate / 0.04) * noise;
  }
  for (std::size_t start = 0; start < kLoopFrames; start += period) {
    for (std::size_t i = 0; i < length && start + i < kLoopFrames; ++i) out[start + i] += hit[i];
  }
  return out;
}

/// Positions between the snare hits, where only silence surrounds a planted defect.
inline std::vector<std::size_t> gaps_between_hits() {
  std::vector<std::size_t> positions;
  for (std::size_t start = 0; start < kLoopFrames; start += kSampleRate) {
    positions.push_back(start + kSampleRate / 2);
    positions.push_back(start + 3 * kSampleRate / 4);
  }
  return positions;
}

inline std::vector<float> to_float(const std::vector<double>& values) {
  return std::vector<float>(values.begin(), values.end());
}

}  // namespace sonare::test::repair_drums
