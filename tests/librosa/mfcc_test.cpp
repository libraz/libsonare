/// @file mfcc_test.cpp
/// @brief librosa compatibility tests for MFCC.
/// @details Reference values from: tests/librosa/reference/mfcc.json

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

#include "core/audio.h"
#include "feature/mel_spectrogram.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates 440Hz sine tone matching librosa.tone().
std::vector<float> create_tone(int sr, float duration, float freq = 440.0f) {
  size_t n_samples = static_cast<size_t>(duration * sr);
  std::vector<float> y(n_samples);

  for (size_t i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / sr;
    y[i] = std::sin(2.0f * M_PI * freq * t);
  }

  return y;
}

/// @brief Computes mean of a range.
float compute_mean(const float* data, size_t size) {
  float sum = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    sum += data[i];
  }
  return sum / size;
}

/// @brief Computes std of a range.
float compute_std(const float* data, size_t size) {
  float mean = compute_mean(data, size);
  float var = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    float diff = data[i] - mean;
    var += diff * diff;
  }
  // Use N-1 for sample std (ddof=1)
  return std::sqrt(var / (size - 1));
}

}  // namespace

TEST_CASE("MFCC librosa compatibility", "[mfcc][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/mfcc.json");
  const auto& data = json["data"].as_array();

  const int sr = 22050;
  const float duration = 1.0f;

  for (const auto& item : data) {
    int n_mfcc = item["n_mfcc"].as_int();
    int n_mels = item["n_mels"].as_int();
    const auto& ref_mean = item["mean"].as_array();
    const auto& ref_std = item["std"].as_array();

    std::string section_name =
        "n_mfcc=" + std::to_string(n_mfcc) + " n_mels=" + std::to_string(n_mels);

    SECTION(section_name) {
      // Create 440Hz tone
      auto samples = create_tone(sr, duration, 440.0f);
      Audio audio = Audio::from_vector(std::move(samples), sr);

      // Compute Mel spectrogram with matching parameters
      MelConfig mel_config;
      mel_config.n_fft = 2048;
      mel_config.hop_length = 512;
      mel_config.n_mels = n_mels;

      MelSpectrogram mel_spec = MelSpectrogram::compute(audio, mel_config);

      // Compute MFCC
      auto mfcc_data = mel_spec.mfcc(n_mfcc);
      int n_frames = mel_spec.n_frames();

      // Verify shape
      REQUIRE(mfcc_data.size() == static_cast<size_t>(n_mfcc * n_frames));

      // Compare mean and std for each coefficient
      for (int c = 0; c < n_mfcc; ++c) {
        const float* coeff_data = mfcc_data.data() + c * n_frames;
        float our_mean = compute_mean(coeff_data, n_frames);
        float our_std = compute_std(coeff_data, n_frames);

        float expected_mean = ref_mean[c].as_float();
        float expected_std = ref_std[c].as_float();

        CAPTURE(c, our_mean, expected_mean, our_std, expected_std);

        // Compare mean and std with librosa reference
        // 10% tolerance for mean, 15% for std
        // (higher coefficients and lower n_mels have more variance)
        if (std::abs(expected_mean) > 1.0f) {
          REQUIRE_THAT(our_mean, WithinRel(expected_mean, 0.1f));
        }
        if (expected_std > 0.1f) {
          REQUIRE_THAT(our_std, WithinRel(expected_std, 0.15f));
        }
      }
    }
  }
}
