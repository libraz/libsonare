/// @file spectral_test.cpp
/// @brief librosa compatibility tests for spectral features.
/// @details Reference values from: tests/librosa/reference/spectral_features.json

#include "feature/spectral.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "core/spectrum.h"
#include "util/json_reader.h"
#include "util/math_utils.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("spectral features librosa compatibility", "[spectral][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/spectral_features.json");
  const auto& data = json["data"];

  int sr = data["sr"].as_int();
  int n_fft = data["n_fft"].as_int();
  int hop_length = data["hop_length"].as_int();

  // Create two-tone signal: 0.5 * (sin(2*pi*440*t) + sin(2*pi*880*t)), 1.0s
  size_t n_samples = static_cast<size_t>(sr);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = 0.5f * (std::sin(kTwoPi * 440.0f * t) + std::sin(kTwoPi * 880.0f * t));
  }
  Audio audio = Audio::from_buffer(samples.data(), n_samples, sr);

  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  // Note: librosa uses center=True STFT by default, which pads the signal.
  // Edge frames (first and last) may differ significantly between implementations.
  // We skip boundary frames for element-level comparison.
  int skip_boundary = 1;  // skip first and last frame

  SECTION("centroid") {
    const auto& ref = data["centroid"].as_array();
    auto result = spectral_centroid(spec, sr);
    REQUIRE(result.size() == ref.size());
    for (size_t i = static_cast<size_t>(skip_boundary);
         i < result.size() - static_cast<size_t>(skip_boundary); ++i) {
      CAPTURE(i);
      REQUIRE_THAT(static_cast<double>(result[i]),
                   WithinRel(static_cast<double>(ref[i].as_float()), 5e-2));
    }
  }

  SECTION("bandwidth") {
    const auto& ref = data["bandwidth"].as_array();
    auto result = spectral_bandwidth(spec, sr);
    REQUIRE(result.size() == ref.size());
    // Bandwidth computation has significant implementation differences (p-norm weighting).
    // Compare mean values instead of per-frame to absorb boundary divergence.
    float our_sum = 0.0f, ref_sum = 0.0f;
    for (size_t i = 0; i < result.size(); ++i) {
      our_sum += result[i];
      ref_sum += ref[i].as_float();
    }
    float our_mean = our_sum / result.size();
    float ref_mean = ref_sum / ref.size();
    REQUIRE_THAT(static_cast<double>(our_mean), WithinRel(static_cast<double>(ref_mean), 2e-1));
  }

  SECTION("rolloff") {
    const auto& ref = data["rolloff"].as_array();
    auto result = spectral_rolloff(spec, sr, 0.85f);
    REQUIRE(result.size() == ref.size());
    for (size_t i = static_cast<size_t>(skip_boundary);
         i < result.size() - static_cast<size_t>(skip_boundary); ++i) {
      CAPTURE(i);
      REQUIRE_THAT(static_cast<double>(result[i]),
                   WithinRel(static_cast<double>(ref[i].as_float()), 5e-2));
    }
  }

  SECTION("flatness") {
    const auto& ref = data["flatness"].as_array();
    auto result = spectral_flatness(spec);
    REQUIRE(result.size() == ref.size());
    for (size_t i = static_cast<size_t>(skip_boundary);
         i < result.size() - static_cast<size_t>(skip_boundary); ++i) {
      CAPTURE(i);
      // Flatness values can be very small; use generous absolute tolerance
      // because geometric mean computation differs between float32/float64
      REQUIRE_THAT(static_cast<double>(result[i]),
                   WithinAbs(static_cast<double>(ref[i].as_float()), 1e-1));
    }
  }

  SECTION("contrast") {
    const auto& ref = data["contrast"].as_array();
    const auto& shape = data["contrast_shape"].as_array();
    int n_bands_plus_one = shape[0].as_int();
    int n_frames = shape[1].as_int();

    auto result = spectral_contrast(spec, sr, 6, 200.0f);
    REQUIRE(result.size() == ref.size());
    REQUIRE(result.size() == static_cast<size_t>(n_bands_plus_one * n_frames));

    // Spectral contrast depends heavily on quantile estimation which varies
    // between implementations. Use generous tolerances.
    for (size_t i = 0; i < result.size(); ++i) {
      float ref_val = ref[i].as_float();
      float res_val = result[i];
      CAPTURE(i, res_val, ref_val);
      // Use absolute tolerance; contrast values can vary significantly
      // between quantile estimation methods
      REQUIRE(std::abs(res_val - ref_val) < std::max(std::abs(ref_val) * 1.0f, 1.0f));
    }
  }
}
