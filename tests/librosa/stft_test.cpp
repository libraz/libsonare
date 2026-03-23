/// @file stft_test.cpp
/// @brief librosa compatibility tests for STFT.
/// @details Reference values from: tests/librosa/reference/stft.json

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "util/json_reader.h"
#include "util/math_utils.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates sine tone matching librosa.tone().
std::vector<float> create_tone(int sr, float duration, float freq) {
  size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) {
    y[i] = std::sin(kTwoPi * freq * static_cast<float>(i) / sr);
  }
  return y;
}

}  // namespace

TEST_CASE("STFT librosa compatibility", "[stft][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/stft.json");
  const auto& data = json["data"].as_array();

  const int sr = 22050;
  const float duration = 1.0f;
  const float freq = 440.0f;

  for (const auto& item : data) {
    int n_fft = item["n_fft"].as_int();
    int hop_length = item["hop_length"].as_int();
    int expected_n_bins = item["shape"][0].as_int();
    int expected_n_frames = item["shape"][1].as_int();
    float expected_mag_sum = item["magnitude_sum"].as_float();
    float expected_mag_max = item["magnitude_max"].as_float();
    const auto& ref_magnitude = item["magnitude"].as_array();

    std::string section_name =
        "n_fft=" + std::to_string(n_fft) + " hop=" + std::to_string(hop_length);

    SECTION(section_name) {
      // Create 440Hz tone
      auto samples = create_tone(sr, duration, freq);
      Audio audio = Audio::from_vector(std::move(samples), sr);

      // Compute STFT
      StftConfig config;
      config.n_fft = n_fft;
      config.hop_length = hop_length;

      Spectrogram spec = Spectrogram::compute(audio, config);

      // Verify shape
      REQUIRE(spec.n_bins() == expected_n_bins);
      REQUIRE(spec.n_bins() == n_fft / 2 + 1);
      REQUIRE(spec.n_frames() == expected_n_frames);

      // Compare magnitude_sum
      const auto& mag = spec.magnitude();
      float mag_sum = 0.0f;
      for (float v : mag) {
        mag_sum += v;
      }
      REQUIRE_THAT(mag_sum, WithinRel(expected_mag_sum, 1e-3f));

      // Compare magnitude_max
      float mag_max = 0.0f;
      for (float v : mag) {
        mag_max = std::max(mag_max, v);
      }
      REQUIRE_THAT(mag_max, WithinRel(expected_mag_max, 1e-3f));

      // Compare full magnitude matrix element-by-element
      // Reference is flattened [n_bins x n_frames] in row-major order
      REQUIRE(ref_magnitude.size() == static_cast<size_t>(expected_n_bins * expected_n_frames));
      REQUIRE(mag.size() == ref_magnitude.size());

      for (size_t i = 0; i < ref_magnitude.size(); ++i) {
        float expected = ref_magnitude[i].as_float();
        float actual = mag[i];
        if (expected > 0.1f) {
          // Use relative tolerance for significant values.
          // Float32 (libsonare) vs float64 (librosa) precision differences
          // in FFT windowing cause up to ~5% divergence for individual elements.
          CAPTURE(i, actual, expected);
          REQUIRE_THAT(actual, WithinRel(expected, 5e-2f));
        } else {
          // Use absolute tolerance for small/near-zero values where
          // float32 precision and windowing differences dominate
          CAPTURE(i, actual, expected);
          REQUIRE(std::abs(actual - expected) < 0.02f);
        }
      }
    }
  }
}
