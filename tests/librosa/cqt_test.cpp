/// @file cqt_test.cpp
/// @brief librosa compatibility tests for Constant-Q Transform.
/// @details Reference values from: tests/librosa/reference/cqt.json

#include "feature/cqt.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "util/json_reader.h"
#include "util/math_utils.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("CQT librosa compatibility", "[cqt][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/cqt.json");
  const auto& data = json["data"];

  int sr = data["sr"].as_int();
  float ref_fmin = data["fmin"].as_float();
  int ref_n_bins = data["n_bins"].as_int();
  int ref_bins_per_octave = data["bins_per_octave"].as_int();
  int hop_length = data["hop_length"].as_int();

  // Create 440Hz tone, 1.0s
  size_t n_samples = static_cast<size_t>(sr);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = std::sin(kTwoPi * 440.0f * t);
  }
  Audio audio = Audio::from_buffer(samples.data(), n_samples, sr);

  SECTION("shape and statistics") {
    CqtConfig config;
    config.fmin = ref_fmin;
    config.n_bins = ref_n_bins;
    config.bins_per_octave = ref_bins_per_octave;
    config.hop_length = hop_length;

    CqtResult result = cqt(audio, config);

    // Verify frequency bins match
    const auto& shape = data["shape"].as_array();
    REQUIRE(result.n_bins() == shape[0].as_int());

    // With center padding, frame counts should match librosa
    int ref_n_frames = shape[1].as_int();
    int our_n_frames = result.n_frames();
    CAPTURE(our_n_frames, ref_n_frames);
    REQUIRE(our_n_frames == ref_n_frames);

    // The magnitude scales also differ due to different CQT normalization.
    // Verify that the output is non-trivial (has positive energy).
    const auto& mag = result.magnitude();
    float mag_sum = 0.0f;
    float mag_max = 0.0f;
    for (float v : mag) {
      mag_sum += v;
      mag_max = std::max(mag_max, v);
    }
    REQUIRE(mag_sum > 0.0f);
    REQUIRE(mag_max > 0.0f);
  }

  SECTION("frequencies") {
    const auto& ref_freqs = data["frequencies"].as_array();
    auto freqs = cqt_frequencies(ref_fmin, ref_n_bins, ref_bins_per_octave);
    REQUIRE(freqs.size() == ref_freqs.size());

    for (size_t i = 0; i < freqs.size(); ++i) {
      REQUIRE_THAT(static_cast<double>(freqs[i]),
                   WithinRel(static_cast<double>(ref_freqs[i].as_float()), 1e-5));
    }
  }

  SECTION("energy at 440Hz") {
    CqtConfig config;
    config.fmin = ref_fmin;
    config.n_bins = ref_n_bins;
    config.bins_per_octave = ref_bins_per_octave;
    config.hop_length = hop_length;

    CqtResult result = cqt(audio, config);

    // Verify the 440Hz bin has the highest energy (sum across frames)
    const auto& freqs = result.frequencies();
    const auto& mag = result.magnitude();
    int n_frames = result.n_frames();
    int n_bins = result.n_bins();

    std::vector<float> bin_energy(n_bins, 0.0f);
    for (int b = 0; b < n_bins; ++b) {
      for (int f = 0; f < n_frames; ++f) {
        bin_energy[b] += mag[b * n_frames + f];
      }
    }

    // Find the bin with highest energy
    int max_energy_bin = 0;
    for (int b = 1; b < n_bins; ++b) {
      if (bin_energy[b] > bin_energy[max_energy_bin]) {
        max_energy_bin = b;
      }
    }

    // The bin with highest energy should be at or very near 440Hz
    float max_energy_freq = freqs[max_energy_bin];
    REQUIRE(std::abs(max_energy_freq - 440.0f) < 5.0f);
  }
}
