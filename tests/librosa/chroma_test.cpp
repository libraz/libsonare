/// @file chroma_test.cpp
/// @brief librosa compatibility tests for chroma features.
/// @details Reference values from: tests/librosa/reference/chroma.json

#include "feature/chroma.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "core/spectrum.h"
#include "util/json_reader.h"
#include "util/math_utils.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("chroma librosa compatibility", "[chroma][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/chroma.json");
  const auto& data = json["data"].as_array();

  constexpr int sr = 22050;
  constexpr float duration = 1.0f;
  size_t n_samples = static_cast<size_t>(sr * duration);

  for (const auto& entry : data) {
    std::string signal_name = entry["signal"].as_string();

    SECTION(signal_name) {
      // Generate signal
      std::vector<float> samples(n_samples);
      if (signal_name == "C_major_chord") {
        for (size_t i = 0; i < n_samples; ++i) {
          float t = static_cast<float>(i) / static_cast<float>(sr);
          samples[i] =
              (1.0f / 3.0f) * (std::sin(kTwoPi * 261.63f * t) + std::sin(kTwoPi * 329.63f * t) +
                               std::sin(kTwoPi * 392.0f * t));
        }
      } else if (signal_name == "440Hz_tone") {
        for (size_t i = 0; i < n_samples; ++i) {
          float t = static_cast<float>(i) / static_cast<float>(sr);
          samples[i] = std::sin(kTwoPi * 440.0f * t);
        }
      } else {
        FAIL("Unknown signal: " + signal_name);
      }
      Audio audio = Audio::from_buffer(samples.data(), n_samples, sr);

      // Compute spectrogram, then chroma from spectrogram
      StftConfig stft_config;
      stft_config.n_fft = 2048;
      stft_config.hop_length = 512;
      Spectrogram spec = Spectrogram::compute(audio, stft_config);
      Chroma chroma = Chroma::from_spectrogram(spec, sr);

      // Verify shape
      const auto& shape = entry["shape"].as_array();
      int expected_n_chroma = shape[0].as_int();
      int expected_n_frames = shape[1].as_int();
      REQUIRE(chroma.n_chroma() == expected_n_chroma);
      REQUIRE(chroma.n_frames() == expected_n_frames);

      // Compare mean energy per pitch class.
      // libsonare's chroma is NOT normalized to [0,1] like librosa's default.
      // Normalize both by dividing by sum to compare relative proportions.
      const auto& ref_mean = entry["mean_per_class"].as_array();
      auto mean_energy = chroma.mean_energy();
      REQUIRE(mean_energy.size() == ref_mean.size());

      // Compute sums for normalization
      float our_sum = 0.0f;
      float ref_sum_val = 0.0f;
      for (size_t c = 0; c < mean_energy.size(); ++c) {
        our_sum += mean_energy[c];
        ref_sum_val += ref_mean[c].as_float();
      }
      REQUIRE(our_sum > 0.0f);
      REQUIRE(ref_sum_val > 0.0f);

      // Compare normalized proportions
      for (size_t c = 0; c < mean_energy.size(); ++c) {
        float our_norm = mean_energy[c] / our_sum;
        float ref_norm = ref_mean[c].as_float() / ref_sum_val;
        CAPTURE(c, our_norm, ref_norm, mean_energy[c], ref_mean[c].as_float());
        REQUIRE_THAT(static_cast<double>(our_norm), WithinAbs(static_cast<double>(ref_norm), 0.15));
      }

      // Verify dominant pitch class (argmax of mean_per_class should match)
      size_t ref_dominant = 0;
      float ref_max = ref_mean[0].as_float();
      for (size_t c = 1; c < ref_mean.size(); ++c) {
        if (ref_mean[c].as_float() > ref_max) {
          ref_max = ref_mean[c].as_float();
          ref_dominant = c;
        }
      }

      size_t our_dominant = 0;
      float our_max = mean_energy[0];
      for (size_t c = 1; c < mean_energy.size(); ++c) {
        if (mean_energy[c] > our_max) {
          our_max = mean_energy[c];
          our_dominant = c;
        }
      }
      REQUIRE(our_dominant == ref_dominant);
    }
  }
}
