/// @file onset_test.cpp
/// @brief Reference compatibility tests for onset strength.
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

/// @brief Creates impulse train signal matching the reference.
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

TEST_CASE("onset strength reference compatibility", "[onset][reference]") {
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
      // Create the same impulse train signal used in the reference
      auto samples = create_impulse_train(sr, 1.0f);
      Audio audio = Audio::from_vector(std::move(samples), sr);

      // Configure Mel spectrogram with standard defaults
      MelConfig mel_config;
      mel_config.n_fft = 2048;
      mel_config.hop_length = hop_length;
      mel_config.n_mels = 128;

      OnsetConfig onset_config;
      onset_config.detrend = false;

      // Compute onset strength
      auto onset_env = compute_onset_strength(audio, mel_config, onset_config);

      // Verify shape
      const auto& ref_shape = item["shape"].as_array();
      REQUIRE(onset_env.size() == static_cast<size_t>(ref_shape[0].as_int()));

      // Compute statistics
      float max_val = 0.0f;
      float sum = 0.0f;
      for (float v : onset_env) {
        max_val = std::max(max_val, v);
        sum += v;
      }
      float mean = sum / onset_env.size();

      REQUIRE(max_val > 0.0f);
      REQUIRE(mean > 0.0f);

      REQUIRE_THAT(max_val, WithinRel(ref_max, 0.2f));
      REQUIRE_THAT(mean, WithinRel(ref_mean, 0.2f));

      const auto& ref_peaks = item["top_peak_frames"].as_array();
      REQUIRE(ref_peaks.size() >= 4);

      std::vector<int> peak_frames;
      for (size_t i = 1; i + 1 < onset_env.size(); ++i) {
        if (onset_env[i] > onset_env[i - 1] && onset_env[i] >= onset_env[i + 1] &&
            onset_env[i] >= max_val * 0.3f) {
          peak_frames.push_back(static_cast<int>(i));
        }
      }
      REQUIRE(peak_frames.size() >= 4);

      for (size_t i = 0; i < 4; ++i) {
        CAPTURE(i, peak_frames[i], ref_peaks[i].as_int());
        REQUIRE(std::abs(peak_frames[i] - ref_peaks[i].as_int()) <= 1);
      }
    }
  }
}
