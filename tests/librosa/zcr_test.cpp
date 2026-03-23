/// @file zcr_test.cpp
/// @brief librosa compatibility tests for ZCR and RMS energy.
/// @details Reference values from: tests/librosa/reference/zcr_rms.json

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "core/audio.h"
#include "feature/spectral.h"
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

/// @brief Creates white noise with seed for reproducibility.
std::vector<float> create_white_noise(int sr, float duration, unsigned int seed) {
  size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  std::mt19937 rng(seed);
  std::normal_distribution<float> dist(0.0f, 1.0f);
  for (size_t i = 0; i < n; ++i) {
    y[i] = dist(rng);
  }
  return y;
}

/// @brief Creates signal by name.
std::vector<float> create_signal(const std::string& name, int sr, float duration) {
  if (name == "440Hz_tone") {
    return create_tone(sr, duration, 440.0f);
  } else if (name == "white_noise") {
    return create_white_noise(sr, duration, 42);
  }
  return {};
}

}  // namespace

TEST_CASE("ZCR/RMS librosa compatibility", "[zcr][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/zcr_rms.json");
  const auto& data = json["data"].as_array();

  for (const auto& item : data) {
    std::string signal_name = item["signal"].as_string();
    int sr = item["sr"].as_int();
    int frame_length = item["frame_length"].as_int();
    int hop_length = item["hop_length"].as_int();

    SECTION("ZCR " + signal_name) {
      const auto& ref_zcr = item["zcr"].as_array();
      int ref_n_frames = static_cast<int>(ref_zcr.size());

      // Create signal
      auto samples = create_signal(signal_name, sr, 1.0f);
      REQUIRE(!samples.empty());
      Audio audio = Audio::from_vector(std::move(samples), sr);

      // Compute ZCR
      auto zcr = zero_crossing_rate(audio, frame_length, hop_length);
      int n_frames = static_cast<int>(zcr.size());

      // librosa uses center=True (pads by frame_length//2), libsonare does not.
      // The offset is frame_length / (2 * hop_length) frames.
      int offset = frame_length / (2 * hop_length);
      int compare_frames = std::min(n_frames, ref_n_frames - offset);
      CAPTURE(signal_name, n_frames, ref_n_frames, offset);
      REQUIRE(compare_frames > 0);

      if (signal_name == "white_noise") {
        // C++ std::mt19937 and numpy use different RNG algorithms, so
        // element-level comparison is meaningless for white noise.
        // Instead, verify statistical properties: ZCR ~0.5 for Gaussian noise.
        float zcr_sum = 0.0f;
        for (int i = 0; i < n_frames; ++i) {
          zcr_sum += zcr[i];
        }
        float zcr_mean = zcr_sum / static_cast<float>(n_frames);
        CAPTURE(signal_name, zcr_mean);
        REQUIRE(std::abs(zcr_mean - 0.5f) < 0.05f);
      } else {
        // Compare ZCR values with offset into reference
        for (int i = 0; i < compare_frames; ++i) {
          float expected = ref_zcr[i + offset].as_float();
          float actual = zcr[i];
          CAPTURE(signal_name, i, actual, expected);
          REQUIRE_THAT(actual, WithinRel(expected, 1e-2f));
        }
      }
    }

    SECTION("RMS " + signal_name) {
      const auto& ref_rms = item["rms"].as_array();
      int ref_n_frames = static_cast<int>(ref_rms.size());

      // Create signal
      auto samples = create_signal(signal_name, sr, 1.0f);
      REQUIRE(!samples.empty());
      Audio audio = Audio::from_vector(std::move(samples), sr);

      // Compute RMS energy
      auto rms = rms_energy(audio, frame_length, hop_length);
      int n_frames = static_cast<int>(rms.size());

      // librosa uses center=True (pads by frame_length//2), libsonare does not.
      int offset = frame_length / (2 * hop_length);
      int compare_frames = std::min(n_frames, ref_n_frames - offset);
      CAPTURE(signal_name, n_frames, ref_n_frames, offset);
      REQUIRE(compare_frames > 0);

      if (signal_name == "white_noise") {
        // C++ std::mt19937 and numpy use different RNG algorithms, so
        // element-level comparison is meaningless for white noise.
        // Instead, verify statistical properties: RMS ~1.0 for unit variance Gaussian.
        float rms_sum = 0.0f;
        for (int i = 0; i < n_frames; ++i) {
          rms_sum += rms[i];
        }
        float rms_mean = rms_sum / static_cast<float>(n_frames);
        CAPTURE(signal_name, rms_mean);
        REQUIRE(std::abs(rms_mean - 1.0f) < 0.1f);
      } else {
        // Compare RMS values with offset into reference
        for (int i = 0; i < compare_frames; ++i) {
          float expected = ref_rms[i + offset].as_float();
          float actual = rms[i];
          CAPTURE(signal_name, i, actual, expected);
          REQUIRE_THAT(actual, WithinRel(expected, 1e-2f));
        }
      }
    }
  }
}
