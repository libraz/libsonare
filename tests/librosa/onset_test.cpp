/// @file onset_test.cpp
/// @brief Reference compatibility tests for onset strength.
/// @details Reference values from: tests/librosa/reference/onset_strength.json
///          and tests/librosa/reference/onset_backtrack.json

#include "feature/onset.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"
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
      float hann = 0.5f * (1.0f - std::cos(2.0f * sonare::constants::kPiD * i / (window_size - 1)));
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

TEST_CASE("onset_backtrack reference compatibility", "[onset][onset_backtrack][reference]") {
  // The oracle for the rule itself. onset_backtrack keeps the RIGHT edge of a
  // non-increasing run -- an index is a local minimum only when
  // energy[i] <= energy[i-1] AND energy[i] < energy[i+1] -- so a plateau stops
  // the search at its last flat sample. A rule that merely walks left while the
  // previous sample is not larger lands on the plateau's first sample instead,
  // which agrees with librosa on strictly monotone curves and disagrees on
  // every flat one. The fixture carries both families plus the degenerate
  // shapes; regenerate it with tests/librosa/generate_librosa_reference.py.
  auto json = JsonReader::parse_file("tests/librosa/reference/onset_backtrack.json");
  const auto& data = json["data"].as_array();
  REQUIRE_FALSE(data.empty());

  for (const auto& item : data) {
    const std::string name = item["name"].as_string();
    CAPTURE(name);

    std::vector<float> energy;
    for (const auto& value : item["energy"].as_array()) {
      energy.push_back(value.as_float());
    }
    std::vector<int> events;
    for (const auto& value : item["events"].as_array()) {
      events.push_back(value.as_int());
    }
    std::vector<int> expected;
    for (const auto& value : item["backtracked"].as_array()) {
      expected.push_back(value.as_int());
    }

    const std::vector<int> actual = onset_backtrack(events, energy);
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
      CAPTURE(i);
      CAPTURE(events[i]);
      REQUIRE(actual[i] == expected[i]);
    }
  }
}
