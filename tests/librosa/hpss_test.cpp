/// @file hpss_test.cpp
/// @brief Reference compatibility tests for HPSS.
/// @details Reference values from: tests/librosa/reference/hpss.json

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "util/json_reader.h"
#include "util/math_utils.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

namespace {

std::vector<float> create_tone_plus_impulses(int sr, float duration) {
  size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n, 0.0f);

  for (size_t i = 0; i < n; ++i) {
    y[i] = 0.5f * std::sin(kTwoPi * 440.0f * static_cast<float>(i) / sr);
  }

  for (float pos : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}) {
    size_t idx = static_cast<size_t>(pos * sr);
    for (size_t i = 0; i < 50 && idx + i < y.size(); ++i) {
      y[idx + i] += 1.0f;
    }
  }

  return y;
}

float sum_values(const std::vector<float>& values) {
  return std::accumulate(values.begin(), values.end(), 0.0f);
}

float max_value(const std::vector<float>& values) {
  return *std::max_element(values.begin(), values.end());
}

float energy_sum(const std::vector<float>& values) {
  float total = 0.0f;
  for (float value : values) {
    total += value * value;
  }
  return total;
}

}  // namespace

TEST_CASE("HPSS reference compatibility", "[hpss][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/hpss.json");
  const auto& item = json["data"];

  int sr = item["sr"].as_int();
  int n_fft = item["n_fft"].as_int();
  int hop_length = item["hop_length"].as_int();
  int expected_n_bins = item["shape"][0].as_int();
  int expected_n_frames = item["shape"][1].as_int();

  auto samples = create_tone_plus_impulses(sr, 1.0f);
  Audio audio = Audio::from_vector(std::move(samples), sr);

  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  HpssSpectrogramResult result = hpss(spec);

  REQUIRE(result.harmonic.n_bins() == expected_n_bins);
  REQUIRE(result.harmonic.n_frames() == expected_n_frames);
  REQUIRE(result.percussive.n_bins() == expected_n_bins);
  REQUIRE(result.percussive.n_frames() == expected_n_frames);

  const std::vector<float>& harmonic = result.harmonic.magnitude();
  const std::vector<float>& percussive = result.percussive.magnitude();
  const std::vector<float>& total = spec.magnitude();

  REQUIRE_THAT(sum_values(harmonic), WithinRel(item["harmonic_sum"].as_float(), 0.10f));
  REQUIRE_THAT(sum_values(percussive), WithinRel(item["percussive_sum"].as_float(), 0.10f));
  REQUIRE_THAT(max_value(harmonic), WithinRel(item["harmonic_max"].as_float(), 0.15f));
  REQUIRE_THAT(max_value(percussive), WithinRel(item["percussive_max"].as_float(), 0.15f));

  float harmonic_ratio = energy_sum(harmonic) / (energy_sum(total) + 1e-10f);
  REQUIRE_THAT(harmonic_ratio, WithinRel(item["harmonic_energy_ratio"].as_float(), 0.05f));
}
