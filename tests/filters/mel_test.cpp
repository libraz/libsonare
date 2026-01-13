/// @file mel_test.cpp
/// @brief Tests for Mel filterbank.

#include "filters/mel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numeric>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("mel_frequencies basic", "[mel]") {
  int n_mels = 40;
  float fmin = 0.0f;
  float fmax = 8000.0f;

  std::vector<float> freqs = mel_frequencies(n_mels, fmin, fmax, false);

  // Should have n_mels + 2 points
  REQUIRE(freqs.size() == static_cast<size_t>(n_mels + 2));

  // First should be fmin
  REQUIRE_THAT(freqs[0], WithinAbs(fmin, 1.0f));

  // Last should be fmax
  REQUIRE_THAT(freqs[n_mels + 1], WithinAbs(fmax, 1.0f));

  // Should be monotonically increasing
  for (size_t i = 1; i < freqs.size(); ++i) {
    REQUIRE(freqs[i] > freqs[i - 1]);
  }
}

TEST_CASE("mel_frequencies HTK vs Slaney", "[mel]") {
  int n_mels = 40;
  float fmin = 0.0f;
  float fmax = 8000.0f;

  std::vector<float> slaney = mel_frequencies(n_mels, fmin, fmax, false);
  std::vector<float> htk = mel_frequencies(n_mels, fmin, fmax, true);

  // Both should have same size
  REQUIRE(slaney.size() == htk.size());

  // But different values (HTK has different formula)
  bool has_difference = false;
  for (size_t i = 1; i < slaney.size() - 1; ++i) {
    if (std::abs(slaney[i] - htk[i]) > 1.0f) {
      has_difference = true;
      break;
    }
  }
  REQUIRE(has_difference);
}

TEST_CASE("create_mel_filterbank dimensions", "[mel]") {
  int sr = 22050;
  int n_fft = 2048;

  MelFilterConfig config;
  config.n_mels = 128;

  std::vector<float> fb = create_mel_filterbank(sr, n_fft, config);

  int n_bins = n_fft / 2 + 1;
  REQUIRE(fb.size() == static_cast<size_t>(config.n_mels * n_bins));
}

TEST_CASE("create_mel_filterbank triangular", "[mel]") {
  int sr = 22050;
  int n_fft = 2048;

  MelFilterConfig config;
  config.n_mels = 40;
  config.norm = MelNorm::None;

  std::vector<float> fb = create_mel_filterbank(sr, n_fft, config);
  int n_bins = n_fft / 2 + 1;

  // Each filter should have non-negative values
  for (float val : fb) {
    REQUIRE(val >= 0.0f);
  }

  // Each filter should have at least one non-zero value
  for (int m = 0; m < config.n_mels; ++m) {
    float max_val = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      max_val = std::max(max_val, fb[m * n_bins + k]);
    }
    REQUIRE(max_val > 0.0f);
  }
}

TEST_CASE("create_mel_filterbank Slaney normalization", "[mel]") {
  int sr = 22050;
  int n_fft = 2048;

  MelFilterConfig config_none;
  config_none.n_mels = 40;
  config_none.norm = MelNorm::None;

  MelFilterConfig config_slaney;
  config_slaney.n_mels = 40;
  config_slaney.norm = MelNorm::Slaney;

  std::vector<float> fb_none = create_mel_filterbank(sr, n_fft, config_none);
  std::vector<float> fb_slaney = create_mel_filterbank(sr, n_fft, config_slaney);

  // Both should have same size
  REQUIRE(fb_none.size() == fb_slaney.size());

  // Slaney normalized filters should have different values
  REQUIRE(fb_none != fb_slaney);

  // Check that normalization is applied (values are scaled)
  float max_none = 0.0f;
  float max_slaney = 0.0f;
  for (size_t i = 0; i < fb_none.size(); ++i) {
    max_none = std::max(max_none, fb_none[i]);
    max_slaney = std::max(max_slaney, fb_slaney[i]);
  }
  // Slaney normalization changes the scale
  REQUIRE(std::abs(max_none - max_slaney) > 0.001f);
}

TEST_CASE("apply_mel_filterbank", "[mel]") {
  int sr = 22050;
  int n_fft = 512;
  int n_bins = n_fft / 2 + 1;
  int n_frames = 10;

  MelFilterConfig config;
  config.n_mels = 40;

  // Create filterbank
  std::vector<float> fb = create_mel_filterbank(sr, n_fft, config);

  // Create fake power spectrum (all ones)
  std::vector<float> power(n_bins * n_frames, 1.0f);

  // Apply filterbank
  std::vector<float> mel_spec =
      apply_mel_filterbank(power.data(), n_bins, n_frames, fb.data(), config.n_mels);

  // Output should have correct dimensions
  REQUIRE(mel_spec.size() == static_cast<size_t>(config.n_mels * n_frames));

  // With all-ones power spectrum, output should reflect filterbank row sums
  for (int m = 0; m < config.n_mels; ++m) {
    float row_sum = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      row_sum += fb[m * n_bins + k];
    }
    // Each frame should have the same value
    for (int t = 0; t < n_frames; ++t) {
      REQUIRE_THAT(mel_spec[m * n_frames + t], WithinAbs(row_sum, 1e-5f));
    }
  }
}
