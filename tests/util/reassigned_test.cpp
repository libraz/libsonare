/// @file reassigned_test.cpp
/// @brief Smoke tests for reassigned_spectrogram.

#include "core/audio.h"
#include "core/spectrum.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

using namespace sonare;

namespace {
std::vector<float> tone(int sr, float duration, float freq, float amp) {
  size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) {
    y[i] = amp * std::sin(constants::kTwoPi * freq * static_cast<float>(i) / sr);
  }
  return y;
}
}  // namespace

TEST_CASE("reassigned_spectrogram returns matching shapes", "[util][reassigned]") {
  Audio audio = Audio::from_vector(tone(22050, 0.5f, 440.0f, 0.5f), 22050);
  StftConfig cfg;
  cfg.n_fft = 1024;
  cfg.hop_length = 256;
  cfg.center = true;
  auto r = reassigned_spectrogram(audio, cfg);
  REQUIRE(r.magnitude.size() == r.times.size());
  REQUIRE(r.magnitude.size() == r.frequencies.size());
  REQUIRE(!r.magnitude.empty());
}

TEST_CASE("reassigned_spectrogram time values are in range", "[util][reassigned]") {
  Audio audio = Audio::from_vector(tone(22050, 0.5f, 440.0f, 0.5f), 22050);
  StftConfig cfg;
  cfg.n_fft = 1024;
  cfg.hop_length = 256;
  cfg.center = true;
  auto r = reassigned_spectrogram(audio, cfg);
  for (float t : r.times) {
    REQUIRE(t >= -1.0f);
    REQUIRE(t <= 2.0f);
  }
}
