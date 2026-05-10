/// @file spectral_test.cpp
/// @brief Reference compatibility tests for spectral features.
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

TEST_CASE("spectral features reference compatibility", "[spectral][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/spectral_features.json");
  const auto& data = json["data"];

  int sr = data["sr"].as_int();
  int n_fft = data["n_fft"].as_int();
  int hop_length = data["hop_length"].as_int();

  // Create two-tone signal: 0.5 * (sin(2*pi*440*t) + sin(2*pi*880*t)), 1.0s
  size_t n_samples = static_cast<size_t>(sr);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    double t = static_cast<double>(i) / static_cast<double>(sr);
    samples[i] = static_cast<float>(
        0.5 * (std::sin(static_cast<double>(kTwoPi) * 440.0 * t) +
               std::sin(static_cast<double>(kTwoPi) * 880.0 * t)));
  }
  Audio audio = Audio::from_buffer(samples.data(), n_samples, sr);

  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  // Centered STFT pads the signal at both edges.
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
                   WithinRel(static_cast<double>(ref[i].as_float()), 1e-2));
    }
  }

  SECTION("bandwidth") {
    const auto& ref = data["bandwidth"].as_array();
    auto result = spectral_bandwidth(spec, sr);
    REQUIRE(result.size() == ref.size());
    for (size_t i = 0; i < result.size(); ++i) {
      CAPTURE(i);
      REQUIRE_THAT(static_cast<double>(result[i]),
                   WithinAbs(static_cast<double>(ref[i].as_float()), 0.45));
    }
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
      REQUIRE_THAT(static_cast<double>(result[i]),
                   WithinAbs(static_cast<double>(ref[i].as_float()), 1e-7));
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

    double mean_abs_diff = 0.0;
    double max_abs_diff = 0.0;
    size_t count = 0;
    for (int b = 0; b < n_bands_plus_one; ++b) {
      for (int t = skip_boundary; t < n_frames - skip_boundary; ++t) {
        size_t i = static_cast<size_t>(b * n_frames + t);
        float ref_val = ref[i].as_float();
        float res_val = result[i];
        double diff = std::abs(static_cast<double>(res_val) - static_cast<double>(ref_val));
        mean_abs_diff += diff;
        max_abs_diff = std::max(max_abs_diff, diff);
        ++count;
      }
    }
    mean_abs_diff /= static_cast<double>(count);

    // Threshold has small platform-dependent slack: macOS observed ~0.9 dB,
    // Linux observed ~1.01 dB. libm differences (cos/log10) and float ordering
    // in std::sort propagate through power_to_db to produce sub-dB drift.
    REQUIRE(mean_abs_diff < 1.2);
    REQUIRE(max_abs_diff < 9.0);
  }
}
