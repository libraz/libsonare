/// @file reassigned_test.cpp
/// @brief Reference compatibility tests for reassigned_spectrogram.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "util/constants.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

TEST_CASE("reassigned_spectrogram shape matches librosa", "[librosa][reassigned]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/reassigned.json");
  const auto& d = json["data"];
  const int sr = d["sr"].as_int();
  const int n_fft = d["n_fft"].as_int();
  const int hop_length = d["hop_length"].as_int();
  const double duration = d["duration"].as_number();
  const float freq = d["freq"].as_float();

  const size_t n_samples = static_cast<size_t>(duration * sr);
  std::vector<float> y(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    y[i] = 0.5f * std::sin(constants::kTwoPi * freq * static_cast<float>(i) / sr);
  }
  Audio audio = Audio::from_vector(std::move(y), sr);

  StftConfig cfg;
  cfg.n_fft = n_fft;
  cfg.hop_length = hop_length;
  cfg.center = true;
  auto r = reassigned_spectrogram(audio, cfg);

  // Shape check against librosa's reported shape.
  const auto& expected_shape = d["mags_shape"].as_array();
  const int n_bins = expected_shape[0].as_int();
  const int n_frames = expected_shape[1].as_int();
  REQUIRE(static_cast<int>(r.magnitude.size()) == n_bins * n_frames);
  REQUIRE(r.magnitude.size() == r.times.size());
  REQUIRE(r.magnitude.size() == r.frequencies.size());

  // For a pure 440 Hz tone, the bin closest to 440 Hz should reassign close to
  // 440 Hz (within a few percent). librosa's algorithm uses the same
  // Auger-Flandrin reassignment so this should hold tightly.
  const float bin_to_hz = static_cast<float>(sr) / n_fft;
  const int target_bin = static_cast<int>(std::round(freq / bin_to_hz));
  const int mid_frame = n_frames / 2;
  const float reassigned = r.frequencies[target_bin * n_frames + mid_frame];
  CAPTURE(target_bin, mid_frame, reassigned, freq);
  REQUIRE(std::abs(reassigned - freq) / freq < 0.05f);
}
