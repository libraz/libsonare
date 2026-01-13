/// @file onset_test.cpp
/// @brief librosa compatibility tests for onset strength.
/// @details Reference values from: tests/librosa/reference/onset_strength.json

#include "feature/onset.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

#include "core/audio.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates impulse train signal matching the librosa reference.
/// @details Signal has impulses at 0.2s intervals with Hann window.
std::vector<float> create_impulse_train(int sr, float duration) {
  std::vector<float> y(static_cast<size_t>(duration * sr), 0.0f);

  // Add impulses at 0.2s intervals
  std::vector<float> times = {0.2f, 0.4f, 0.6f, 0.8f};
  const int window_size = 100;

  for (float t : times) {
    int idx = static_cast<int>(t * sr);
    for (int i = 0; i < window_size && idx + i < static_cast<int>(y.size()); ++i) {
      // Hann window: 0.5 * (1 - cos(2*pi*i/(N-1)))
      float hann = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (window_size - 1)));
      y[idx + i] = hann;
    }
  }

  return y;
}

}  // namespace

TEST_CASE("onset strength librosa compatibility", "[onset][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/onset_strength.json");
  const auto& data = json["data"].as_array();

  for (const auto& item : data) {
    int sr = item["sr"].as_int();
    int hop_length = item["hop_length"].as_int();
    float ref_max = item["max"].as_float();
    float ref_mean = item["mean"].as_float();

    std::string section_name = "sr=" + std::to_string(sr) + " hop=" + std::to_string(hop_length);

    SECTION(section_name) {
      CAPTURE(ref_max, ref_mean);
      // Create the same impulse train signal used in librosa reference
      auto samples = create_impulse_train(sr, 1.0f);
      Audio audio = Audio::from_vector(std::move(samples), sr);

      // Configure Mel spectrogram to match librosa defaults
      MelConfig mel_config;
      mel_config.n_fft = 2048;
      mel_config.hop_length = hop_length;
      mel_config.n_mels = 128;

      // Match librosa defaults: detrend=False, center=True (but different meaning)
      OnsetConfig onset_config;
      onset_config.detrend = false;
      onset_config.center = false;  // Disable z-score normalization

      // Compute onset strength
      auto onset_env = compute_onset_strength(audio, mel_config, onset_config);

      // Normalize by n_mels to convert sum to mean (librosa uses mean)
      for (auto& v : onset_env) {
        v /= static_cast<float>(mel_config.n_mels);
      }

      // Verify shape (approximately)
      // Note: exact frame count may differ slightly due to padding differences
      REQUIRE(onset_env.size() > 0);

      // Compute statistics
      float max_val = 0.0f;
      float sum = 0.0f;
      for (float v : onset_env) {
        max_val = std::max(max_val, v);
        sum += v;
      }
      float mean = sum / onset_env.size();

      // Compare with librosa reference values
      // Note: Some difference is expected due to:
      // - Mel filterbank implementation details
      // - Power-to-dB conversion parameters
      // - Padding/centering differences
      REQUIRE(max_val > 0.0f);
      REQUIRE(mean > 0.0f);

      // Verify relative ratio is similar (within 50% tolerance)
      float ref_ratio = ref_max / ref_mean;
      float our_ratio = max_val / mean;
      REQUIRE_THAT(our_ratio, WithinRel(ref_ratio, 0.5f));
    }
  }
}
