/// @file poly_features_test.cpp
/// @brief Unit tests for spectral polynomial features.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "feature/spectral.h"
#include "util/constants.h"

using namespace sonare;
using namespace sonare::constants;
using Catch::Matchers::WithinAbs;

namespace {

Audio make_tone(float freq, int sr, float duration) {
  const size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sr);
    y[i] = static_cast<float>(
        std::sin(static_cast<double>(constants::kTwoPi) * static_cast<double>(freq) * t));
  }
  return Audio::from_vector(std::move(y), sr);
}

}  // namespace

TEST_CASE("poly_features returns expected shape", "[poly_features][unit]") {
  Audio audio = make_tone(440.0f, 22050, 0.5f);
  StftConfig cfg;
  cfg.n_fft = 2048;
  cfg.hop_length = 512;
  Spectrogram spec = Spectrogram::compute(audio, cfg);

  const int order = 1;
  auto coeffs = poly_features(spec, audio.sample_rate(), order);
  REQUIRE(coeffs.size() == static_cast<size_t>((order + 1) * spec.n_frames()));
}

TEST_CASE("poly_features higher order returns more coefficients", "[poly_features][unit]") {
  Audio audio = make_tone(440.0f, 22050, 0.5f);
  StftConfig cfg;
  cfg.n_fft = 1024;
  cfg.hop_length = 256;
  Spectrogram spec = Spectrogram::compute(audio, cfg);

  auto c0 = poly_features(spec, audio.sample_rate(), 0);
  auto c1 = poly_features(spec, audio.sample_rate(), 1);
  auto c2 = poly_features(spec, audio.sample_rate(), 2);
  REQUIRE(c0.size() == static_cast<size_t>(spec.n_frames()));
  REQUIRE(c1.size() == static_cast<size_t>(2 * spec.n_frames()));
  REQUIRE(c2.size() == static_cast<size_t>(3 * spec.n_frames()));
}

TEST_CASE("poly_features raw-pointer overload matches", "[poly_features][unit]") {
  Audio audio = make_tone(440.0f, 22050, 0.5f);
  StftConfig cfg;
  cfg.n_fft = 1024;
  cfg.hop_length = 256;
  Spectrogram spec = Spectrogram::compute(audio, cfg);

  auto c_obj = poly_features(spec, audio.sample_rate(), 1);
  auto c_raw = poly_features(spec.magnitude().data(), spec.n_bins(), spec.n_frames(),
                             audio.sample_rate(), spec.n_fft(), 1);
  REQUIRE(c_obj.size() == c_raw.size());
  for (size_t i = 0; i < c_obj.size(); ++i) {
    REQUIRE_THAT(c_obj[i], WithinAbs(c_raw[i], 1e-5f));
  }
}
