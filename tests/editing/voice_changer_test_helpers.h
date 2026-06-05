/// @file voice_changer_test_helpers.h
/// @brief Shared helpers for voice changer tests.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/audio.h"
#include "core/fft.h"
#include "editing/voice_changer/formant_bounds.h"
#include "editing/voice_changer/formant_warp.h"
#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/streaming_reverb.h"
#include "editing/voice_changer/voice_changer.h"
#include "metering/true_peak.h"
#include "sonare_c.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json.h"

using Catch::Matchers::WithinRel;
using namespace sonare::editing::voice_changer;

using sonare::constants::kPi;
using sonare::constants::kTwoPi;

namespace {

[[maybe_unused]] std::vector<float> sine(float frequency_hz, int sample_rate, int samples) {
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) {
    output[static_cast<size_t>(i)] =
        0.5f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz *
                                           static_cast<double>(i) / sample_rate));
  }
  return output;
}

[[maybe_unused]] int zero_crossings(const std::vector<float>& samples) {
  int crossings = 0;
  for (size_t i = 1; i < samples.size(); ++i) {
    if ((samples[i - 1] <= 0.0f && samples[i] > 0.0f) ||
        (samples[i - 1] >= 0.0f && samples[i] < 0.0f)) {
      ++crossings;
    }
  }
  return crossings;
}

// Magnitude-weighted spectral centroid (Hz) of a windowed signal segment.
[[maybe_unused]] float spectral_centroid(const std::vector<float>& samples, int sample_rate) {
  constexpr int kNfft = 4096;
  std::vector<float> frame(static_cast<size_t>(kNfft), 0.0f);
  const int n = std::min(kNfft, static_cast<int>(samples.size()));
  for (int i = 0; i < n; ++i) {
    // Hann window to limit spectral leakage.
    const float w = 0.5f - 0.5f * std::cos(sonare::constants::kTwoPi * static_cast<float>(i) /
                                           static_cast<float>(kNfft - 1));
    frame[static_cast<size_t>(i)] = samples[static_cast<size_t>(i)] * w;
  }
  sonare::FFT fft(kNfft);
  std::vector<std::complex<float>> spec(static_cast<size_t>(fft.n_bins()));
  fft.forward(frame.data(), spec.data());

  double weighted = 0.0;
  double total = 0.0;
  const double bin_hz = static_cast<double>(sample_rate) / kNfft;
  for (int b = 0; b < fft.n_bins(); ++b) {
    const double mag = std::abs(spec[static_cast<size_t>(b)]);
    weighted += mag * (b * bin_hz);
    total += mag;
  }
  return total > 0.0 ? static_cast<float>(weighted / total) : 0.0f;
}

// Dominant fundamental frequency (Hz) via autocorrelation peak search.
[[maybe_unused]] float dominant_frequency(const std::vector<float>& samples, int sample_rate,
                                          float fmin, float fmax) {
  const int min_lag = static_cast<int>(static_cast<float>(sample_rate) / fmax);
  const int max_lag = static_cast<int>(static_cast<float>(sample_rate) / fmin);
  const int n = static_cast<int>(samples.size());
  double best = -1.0;
  int best_lag = min_lag;
  for (int lag = min_lag; lag <= max_lag && lag < n; ++lag) {
    double acc = 0.0;
    for (int i = 0; i + lag < n; ++i) {
      acc += static_cast<double>(samples[static_cast<size_t>(i)]) *
             static_cast<double>(samples[static_cast<size_t>(i + lag)]);
    }
    if (acc > best) {
      best = acc;
      best_lag = lag;
    }
  }
  return static_cast<float>(sample_rate) / static_cast<float>(best_lag);
}

[[maybe_unused]] float block_rms(const std::vector<float>& samples, std::size_t start,
                                 std::size_t end) {
  if (end <= start) return 0.0f;
  double acc = 0.0;
  for (std::size_t i = start; i < end; ++i) acc += samples[i] * samples[i];
  return static_cast<float>(std::sqrt(acc / static_cast<double>(end - start)));
}

}  // namespace
