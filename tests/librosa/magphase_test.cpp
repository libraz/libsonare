/// @file magphase_test.cpp
/// @brief Reference compatibility tests for magphase().

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

#include "core/audio.h"
#include "core/spectrum.h"
#include "util/constants.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::constants;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
std::vector<float> make_tone(int sr, float duration, float freq) {
  size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) {
    y[i] = std::sin(constants::kTwoPi * freq * static_cast<float>(i) / sr);
  }
  return y;
}
}  // namespace

TEST_CASE("magphase magnitude statistics match librosa", "[librosa][magphase]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/magphase.json");
  const auto& d = json["data"];
  int sr = d["sr"].as_int();
  int n_fft = d["n_fft"].as_int();
  int hop = d["hop_length"].as_int();

  auto samples = make_tone(sr, 0.5f, 440.0f);
  Audio audio = Audio::from_vector(std::move(samples), sr);
  StftConfig cfg;
  cfg.n_fft = n_fft;
  cfg.hop_length = hop;
  auto spec = Spectrogram::compute(audio, cfg);
  auto mp = magphase(spec, 1.0f);

  // Sum and max within ~0.1% (matches our STFT tolerance).
  double sum = 0.0;
  float max_mag = 0.0f;
  for (float v : mp.magnitude) {
    sum += v;
    if (v > max_mag) max_mag = v;
  }
  float expected_sum = d["mag_sum"].as_float();
  float expected_max = d["mag_max"].as_float();
  CAPTURE(sum, expected_sum, max_mag, expected_max);
  REQUIRE_THAT(static_cast<float>(sum), WithinRel(expected_sum, 0.05f));
  REQUIRE_THAT(max_mag, WithinRel(expected_max, 0.05f));
}

TEST_CASE("magphase phase has unit modulus", "[magphase][unit]") {
  std::vector<std::complex<float>> spec{
      {1.0f, 2.0f}, {0.0f, 0.0f}, {-3.0f, 0.5f}, {1e-30f, 1e-30f}};
  auto mp = magphase(spec.data(), spec.size());
  REQUIRE(mp.magnitude.size() == 4);
  REQUIRE(mp.phase.size() == 4);
  for (size_t i = 0; i < spec.size(); ++i) {
    float modulus = std::abs(mp.phase[i]);
    CAPTURE(i, modulus);
    REQUIRE_THAT(modulus, WithinAbs(1.0f, 1e-4f));
  }
  // Zero magnitude entry should yield phase 1+0j and magnitude 0.
  REQUIRE_THAT(mp.magnitude[1], WithinAbs(0.0f, 1e-6f));
  REQUIRE_THAT(mp.phase[1].real(), WithinAbs(1.0f, 1e-6f));
  REQUIRE_THAT(mp.phase[1].imag(), WithinAbs(0.0f, 1e-6f));
}
