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

TEST_CASE("get_mel_filterbank_cached returns same matrix for same key", "[mel][cache]") {
  // Cache hit: identical (sr, n_fft, config) must return the same underlying
  // storage (same data pointer) — that is the whole point of caching.
  int sr = 22050;
  int n_fft = 2048;
  MelFilterConfig config;
  config.n_mels = 64;

  const std::vector<float>& a = get_mel_filterbank_cached(sr, n_fft, config);
  const std::vector<float>& b = get_mel_filterbank_cached(sr, n_fft, config);
  REQUIRE(a.data() == b.data());
  REQUIRE(a.size() == static_cast<size_t>(config.n_mels * (n_fft / 2 + 1)));

  // The cached values must match a freshly built filterbank.
  std::vector<float> fresh = create_mel_filterbank(sr, n_fft, config);
  REQUIRE(a.size() == fresh.size());
  for (size_t i = 0; i < fresh.size(); ++i) {
    REQUIRE_THAT(a[i], WithinAbs(fresh[i], 1e-6f));
  }
}

TEST_CASE("get_mel_filterbank_cached treats fmax=0 as sr/2", "[mel][cache]") {
  // 0 sentinel and the explicit sr/2 value must collapse to the same cache key,
  // otherwise the cache silently doubles in size at the most common call sites.
  int sr = 22050;
  int n_fft = 2048;
  MelFilterConfig config_default;
  config_default.n_mels = 32;
  MelFilterConfig config_explicit = config_default;
  config_explicit.fmax = static_cast<float>(sr) / 2.0f;

  const std::vector<float>& a = get_mel_filterbank_cached(sr, n_fft, config_default);
  const std::vector<float>& b = get_mel_filterbank_cached(sr, n_fft, config_explicit);
  REQUIRE(a.data() == b.data());
}

TEST_CASE("get_mel_filterbank_cached distinguishes different keys", "[mel][cache]") {
  int sr = 22050;
  int n_fft = 2048;

  MelFilterConfig c1;
  c1.n_mels = 40;
  MelFilterConfig c2;
  c2.n_mels = 64;
  MelFilterConfig c3;
  c3.n_mels = 40;
  c3.htk = true;

  const std::vector<float>& a = get_mel_filterbank_cached(sr, n_fft, c1);
  const std::vector<float>& b = get_mel_filterbank_cached(sr, n_fft, c2);
  const std::vector<float>& c = get_mel_filterbank_cached(sr, n_fft, c3);

  REQUIRE(a.data() != b.data());
  REQUIRE(a.data() != c.data());
  REQUIRE(b.data() != c.data());
  REQUIRE(a.size() != b.size());  // different n_mels → different shape
}

TEST_CASE("get_mel_filterbank_cached evicts oldest entries past capacity", "[mel][cache]") {
  // The implementation caps the cache at a small constant (kMaxMelCacheSize =
  // 8 at the time of writing). Insert more than that and verify the oldest
  // entry is evicted (its pointer becomes stale on re-fetch). We do not pin
  // the exact capacity from the test — we only require eviction within a
  // reasonable upper bound so the test stays robust if the cap changes.
  int sr = 22050;
  int n_fft = 1024;

  MelFilterConfig first;
  first.n_mels = 16;
  const void* first_ptr = get_mel_filterbank_cached(sr, n_fft, first).data();

  constexpr int kPressure = 32;
  for (int i = 0; i < kPressure; ++i) {
    MelFilterConfig c;
    c.n_mels = 17 + i;
    // Touch every distinct key once so they enter the LRU queue ahead of
    // `first` (which has not been re-touched since).
    (void)get_mel_filterbank_cached(sr, n_fft, c);
  }

  // After kPressure unique inserts, the original entry must have been evicted
  // and rebuilt — pointers cannot match if the storage was freed.
  const void* first_ptr_after = get_mel_filterbank_cached(sr, n_fft, first).data();
  REQUIRE(first_ptr_after != first_ptr);
}
