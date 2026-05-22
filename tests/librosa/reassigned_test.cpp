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
  // 440 Hz. librosa's algorithm uses the same Auger-Flandrin reassignment so
  // this holds tightly.
  const float bin_to_hz = static_cast<float>(sr) / n_fft;
  const int target_bin = static_cast<int>(std::round(freq / bin_to_hz));
  const int mid_frame = n_frames / 2;
  const float reassigned = r.frequencies[target_bin * n_frames + mid_frame];
  CAPTURE(target_bin, mid_frame, reassigned, freq);
  REQUIRE(std::abs(reassigned - freq) / freq < 0.02f);

  // Compare full mid-frame columns of magnitudes, times, and reassigned
  // frequencies against librosa. librosa fills NaNs with 0 for storage.
  const auto& ref_mags = d["mags_row_center"].as_array();
  const auto& ref_freqs = d["freqs_row_center"].as_array();
  const auto& ref_times = d["times_row_center"].as_array();
  REQUIRE(static_cast<int>(ref_mags.size()) == n_bins);

  double mag_sum_sq_diff = 0.0;
  double mag_sum_sq_ref = 0.0;
  double max_freq_abs_diff = 0.0;
  double max_time_abs_diff = 0.0;
  const float center_time = static_cast<float>(mid_frame * hop_length) / sr;
  // Low-magnitude bins reassign chaotically; librosa replaces them with 0 for
  // storage, so only compare bins where the tone has real energy.
  constexpr float kMagThreshold = 0.5f;
  for (int b = 0; b < n_bins; ++b) {
    const float our_mag = r.magnitude[b * n_frames + mid_frame];
    const float ref_mag = ref_mags[b].as_float();
    const double dm = static_cast<double>(our_mag) - ref_mag;
    mag_sum_sq_diff += dm * dm;
    mag_sum_sq_ref += static_cast<double>(ref_mag) * ref_mag;
    if (ref_mag <= kMagThreshold) continue;

    const float our_freq = r.frequencies[b * n_frames + mid_frame];
    const float ref_freq = ref_freqs[b].as_float();
    if (std::isfinite(our_freq)) {
      max_freq_abs_diff =
          std::max(max_freq_abs_diff, static_cast<double>(std::abs(our_freq - ref_freq)));
    }
    const float our_time = r.times[b * n_frames + mid_frame];
    const float ref_time = ref_times[b].as_float();
    if (std::isfinite(our_time)) {
      max_time_abs_diff =
          std::max(max_time_abs_diff, static_cast<double>(std::abs(our_time - ref_time)));
      REQUIRE(std::abs(our_time - center_time) < 0.05f);
    }
  }
  const double rel_mag_frob = std::sqrt(mag_sum_sq_diff / std::max(mag_sum_sq_ref, 1e-12));
  CAPTURE(rel_mag_frob, max_freq_abs_diff, max_time_abs_diff);
  // STFT magnitudes for the centre column should match librosa within float
  // precision (the residual gap is from window-edge sample padding).
  REQUIRE(rel_mag_frob < 5e-3);
  // Reassigned frequencies on bins with real energy match within ~1 Hz.
  REQUIRE(max_freq_abs_diff < 2.0);
  // Reassigned times on bins with real energy match within ~1 ms.
  REQUIRE(max_time_abs_diff < 2e-3);
}
