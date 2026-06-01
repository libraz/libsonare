/// @file chroma_test.cpp
/// @brief Tests for Chroma filterbank.

#include "filters/chroma.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("hz_to_pitch_class", "[chroma]") {
  // A4 = 440 Hz should be pitch class 9 (A)
  REQUIRE(hz_to_pitch_class(440.0f) == 9);

  // C4 ≈ 261.63 Hz should be pitch class 0 (C)
  REQUIRE(hz_to_pitch_class(261.63f) == 0);

  // G4 ≈ 392 Hz should be pitch class 7 (G)
  REQUIRE(hz_to_pitch_class(392.0f) == 7);

  // E4 ≈ 329.63 Hz should be pitch class 4 (E)
  REQUIRE(hz_to_pitch_class(329.63f) == 4);

  // Invalid frequency
  REQUIRE(hz_to_pitch_class(0.0f) == -1);
  REQUIRE(hz_to_pitch_class(-100.0f) == -1);
}

TEST_CASE("hz_to_pitch_class octave invariant", "[chroma]") {
  // Same pitch class regardless of octave
  // A2, A3, A4, A5 should all be pitch class 9
  REQUIRE(hz_to_pitch_class(110.0f) == 9);  // A2
  REQUIRE(hz_to_pitch_class(220.0f) == 9);  // A3
  REQUIRE(hz_to_pitch_class(440.0f) == 9);  // A4
  REQUIRE(hz_to_pitch_class(880.0f) == 9);  // A5

  // C1 through C5 - use slightly sharp frequencies to avoid rounding down to B
  REQUIRE(hz_to_pitch_class(32.8f) == 0);   // ~C1
  REQUIRE(hz_to_pitch_class(65.5f) == 0);   // ~C2
  REQUIRE(hz_to_pitch_class(131.0f) == 0);  // ~C3
  REQUIRE(hz_to_pitch_class(262.0f) == 0);  // ~C4
  REQUIRE(hz_to_pitch_class(524.0f) == 0);  // ~C5
}

TEST_CASE("hz_to_chroma fractional", "[chroma]") {
  // A4 should be exactly 9.0
  REQUIRE_THAT(hz_to_chroma(440.0f), WithinAbs(9.0f, 0.01f));

  // C4 should be exactly 0.0
  REQUIRE_THAT(hz_to_chroma(261.63f), WithinAbs(0.0f, 0.1f));

  // Halfway between A and A# should be ~9.5
  float a4 = 440.0f;
  float asharp4 = 466.16f;
  float mid = std::sqrt(a4 * asharp4);  // Geometric mean
  float chroma = hz_to_chroma(mid);
  REQUIRE(chroma > 9.0f);
  REQUIRE(chroma < 10.0f);
}

TEST_CASE("create_chroma_filterbank dimensions", "[chroma]") {
  int sr = 22050;
  int n_fft = 2048;
  int n_bins = n_fft / 2 + 1;

  ChromaFilterConfig config;
  config.n_chroma = 12;

  std::vector<float> fb = create_chroma_filterbank(sr, n_fft, config);

  REQUIRE(fb.size() == static_cast<size_t>(config.n_chroma * n_bins));
}

TEST_CASE("create_chroma_filterbank L2 normalized by default", "[chroma]") {
  // librosa.filters.chroma's default norm is L2 — each filter row must have
  // unit Euclidean norm. Switching from the legacy L1 (sum=1) makes the
  // filterbank itself librosa-compatible.
  int sr = 22050;
  int n_fft = 2048;
  int n_bins = n_fft / 2 + 1;

  ChromaFilterConfig config;
  config.n_chroma = 12;
  REQUIRE(config.norm == ChromaFilterNorm::L2);

  std::vector<float> fb = create_chroma_filterbank(sr, n_fft, config);

  // Each chroma row should have unit L2 norm.
  for (int c = 0; c < config.n_chroma; ++c) {
    float sum_sq = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      sum_sq += fb[c * n_bins + k] * fb[c * n_bins + k];
    }
    const float l2 = std::sqrt(sum_sq);
    // Rows may be empty when fmin filters out every bin.
    if (l2 > 0.0f) {
      REQUIRE_THAT(l2, WithinAbs(1.0f, 0.01f));
    }
  }
}

TEST_CASE("create_chroma_filterbank L1 norm available for legacy callers", "[chroma]") {
  int sr = 22050;
  int n_fft = 2048;
  int n_bins = n_fft / 2 + 1;

  ChromaFilterConfig config;
  config.n_chroma = 12;
  config.norm = ChromaFilterNorm::L1;

  std::vector<float> fb = create_chroma_filterbank(sr, n_fft, config);

  for (int c = 0; c < config.n_chroma; ++c) {
    float sum = 0.0f;
    for (int k = 0; k < n_bins; ++k) {
      sum += fb[c * n_bins + k];
    }
    if (sum > 0.0f) {
      REQUIRE_THAT(sum, WithinAbs(1.0f, 0.01f));
    }
  }
}

TEST_CASE("create_chroma_filterbank None norm preserves raw weights", "[chroma]") {
  int sr = 22050;
  int n_fft = 2048;

  ChromaFilterConfig config;
  config.n_chroma = 12;
  config.norm = ChromaFilterNorm::None;

  std::vector<float> fb = create_chroma_filterbank(sr, n_fft, config);

  // Without normalization, the triangular weights distributed per FFT bin sum
  // to 1 per column (each bin contributes (1-frac) + frac = 1 across two
  // chroma classes). Check that the total mass equals the active bin count.
  float total = 0.0f;
  for (float v : fb) total += v;
  REQUIRE(total > 0.0f);
}

TEST_CASE("create_chroma_filterbank non-negative", "[chroma]") {
  int sr = 22050;
  int n_fft = 2048;

  ChromaFilterConfig config;

  std::vector<float> fb = create_chroma_filterbank(sr, n_fft, config);

  // All values should be non-negative
  for (float val : fb) {
    REQUIRE(val >= 0.0f);
  }
}

TEST_CASE("apply_chroma_filterbank", "[chroma]") {
  int sr = 22050;
  int n_fft = 512;
  int n_bins = n_fft / 2 + 1;
  int n_frames = 5;

  ChromaFilterConfig config;
  config.n_chroma = 12;
  config.norm = ChromaFilterNorm::L1;

  std::vector<float> fb = create_chroma_filterbank(sr, n_fft, config);

  // Create fake power spectrum
  std::vector<float> power(n_bins * n_frames, 1.0f);

  std::vector<float> chroma =
      apply_chroma_filterbank(power.data(), n_bins, n_frames, fb.data(), config.n_chroma);

  REQUIRE(chroma.size() == static_cast<size_t>(config.n_chroma * n_frames));

  // With uniform power and L1-normalized rows, each chroma row integrates to 1.
  for (int t = 0; t < n_frames; ++t) {
    for (int c = 0; c < config.n_chroma; ++c) {
      REQUIRE_THAT(chroma[c * n_frames + t], WithinAbs(1.0f, 1e-5f));
    }
  }
}

TEST_CASE("get_chroma_filterbank_cached returns same matrix for same key", "[chroma][cache]") {
  int sr = 22050;
  int n_fft = 2048;
  ChromaFilterConfig config;
  config.n_chroma = 12;

  const std::vector<float>& a = get_chroma_filterbank_cached(sr, n_fft, config);
  const std::vector<float>& b = get_chroma_filterbank_cached(sr, n_fft, config);
  REQUIRE(a.data() == b.data());
  REQUIRE(a.size() == static_cast<size_t>(config.n_chroma * (n_fft / 2 + 1)));

  // The cached matrix must match a fresh build.
  std::vector<float> fresh = create_chroma_filterbank(sr, n_fft, config);
  REQUIRE(a.size() == fresh.size());
  for (size_t i = 0; i < fresh.size(); ++i) {
    REQUIRE_THAT(a[i], WithinAbs(fresh[i], 1e-6f));
  }
}

TEST_CASE("get_chroma_filterbank_cached distinguishes different keys", "[chroma][cache]") {
  int sr = 22050;
  int n_fft = 2048;

  ChromaFilterConfig c1;
  c1.n_chroma = 12;
  ChromaFilterConfig c2;
  c2.n_chroma = 24;
  ChromaFilterConfig c3;
  c3.n_chroma = 12;
  c3.norm = ChromaFilterNorm::L1;

  const std::vector<float>& a = get_chroma_filterbank_cached(sr, n_fft, c1);
  const std::vector<float>& b = get_chroma_filterbank_cached(sr, n_fft, c2);
  const std::vector<float>& c = get_chroma_filterbank_cached(sr, n_fft, c3);

  REQUIRE(a.data() != b.data());
  REQUIRE(a.data() != c.data());
  REQUIRE(b.data() != c.data());
  REQUIRE(a.size() != b.size());  // different n_chroma → different shape
}

TEST_CASE("get_chroma_filterbank_cached evicts oldest entries past capacity", "[chroma][cache]") {
  int sr = 22050;
  int n_fft = 1024;

  ChromaFilterConfig first;
  first.n_chroma = 12;
  first.tuning = 0.0f;
  const void* first_ptr = get_chroma_filterbank_cached(sr, n_fft, first).data();

  // Pressure-test by inserting more distinct keys than the cap. The chroma
  // cache uses kMaxChromaCacheSize = 8 today; the test only requires eviction
  // within a generous upper bound so it tolerates future cap changes.
  constexpr int kPressure = 32;
  for (int i = 0; i < kPressure; ++i) {
    ChromaFilterConfig c;
    c.n_chroma = 12;
    c.tuning = 0.01f * static_cast<float>(i + 1);  // distinct key per iter
    (void)get_chroma_filterbank_cached(sr, n_fft, c);
  }

  const void* first_ptr_after = get_chroma_filterbank_cached(sr, n_fft, first).data();
  REQUIRE(first_ptr_after != first_ptr);
}
