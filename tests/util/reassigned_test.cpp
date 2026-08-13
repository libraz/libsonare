/// @file reassigned_test.cpp
/// @brief Smoke tests for reassigned_spectrogram.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
using namespace sonare::constants;

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

TEST_CASE("reassigned_spectrogram rejects a zero n_fft or hop_length instead of dividing by zero",
          "[util][reassigned]") {
  // stft_with_window divides the frame count by hop_length; without this
  // guard hop_length == 0 is an integer division by zero (SIGFPE) instead of
  // a recoverable SonareException. This is the core-level counterpart of the
  // C-ABI check in sonare_reassigned_spectrogram: WASM calls
  // reassigned_spectrogram() directly and does not link the C-ABI TU, so the
  // guard must live here to protect that surface too.
  Audio audio = Audio::from_vector(tone(22050, 0.5f, 440.0f, 0.5f), 22050);
  StftConfig zero_hop;
  zero_hop.n_fft = 1024;
  zero_hop.hop_length = 0;
  zero_hop.center = true;
  REQUIRE_THROWS_AS(reassigned_spectrogram(audio, zero_hop), SonareException);

  StftConfig zero_fft;
  zero_fft.n_fft = 0;
  zero_fft.hop_length = 256;
  zero_fft.center = true;
  REQUIRE_THROWS_AS(reassigned_spectrogram(audio, zero_fft), SonareException);
}
