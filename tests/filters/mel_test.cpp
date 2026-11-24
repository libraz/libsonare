/// @file mel_test.cpp
/// @brief Tests for Mel filterbank.

#include "filters/mel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("mel_frequency_points basic", "[mel]") {
  int n_points = 42;
  float fmin = 0.0f;
  float fmax = 8000.0f;

  std::vector<float> freqs = mel_frequency_points(n_points, fmin, fmax, false);

  // Exactly n_points points, endpoints included
  REQUIRE(freqs.size() == static_cast<size_t>(n_points));

  // First should be fmin
  REQUIRE_THAT(freqs[0], WithinAbs(fmin, 1.0f));

  // Last should be fmax
  REQUIRE_THAT(freqs[n_points - 1], WithinAbs(fmax, 1.0f));

  // Should be monotonically increasing
  for (size_t i = 1; i < freqs.size(); ++i) {
    REQUIRE(freqs[i] > freqs[i - 1]);
  }
}

TEST_CASE("mel_frequency_points with one or two points", "[mel]") {
  for (const bool htk : {false, true}) {
    INFO("htk " << htk);
    // One point is the start alone, as an equally spaced run of one is.
    const std::vector<float> one = mel_frequency_points(1, 300.0f, 8000.0f, htk);
    REQUIRE(one.size() == 1);
    CHECK(std::isfinite(one[0]));
    CHECK_THAT(one[0], WithinRel(300.0f, 1e-4f));
    const std::vector<float> two = mel_frequency_points(2, 300.0f, 8000.0f, htk);
    REQUIRE(two.size() == 2);
    CHECK_THAT(two[0], WithinRel(300.0f, 1e-4f));
    CHECK_THAT(two[1], WithinRel(8000.0f, 1e-4f));
  }
}

TEST_CASE("mel_frequency_points HTK vs Slaney", "[mel]") {
  int n_points = 42;
  float fmin = 0.0f;
  float fmax = 8000.0f;

  std::vector<float> slaney = mel_frequency_points(n_points, fmin, fmax, false);
  std::vector<float> htk = mel_frequency_points(n_points, fmin, fmax, true);

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

TEST_CASE("Mel filterbank refusals name their input", "[mel]") {
  const float sample = 1.0f;
  MelFilterConfig config;
  CHECK_THROWS_WITH(create_mel_filterbank(0, 2048, config),
                    "sample_rate must be an integer in [1, 2147483647]");
  CHECK_THROWS_WITH(create_mel_filterbank(22050, 0, config),
                    "n_fft must be an integer in [1, 2147483647]");

  CHECK_THROWS_WITH(apply_mel_filterbank(nullptr, 1, 1, &sample, 1), "power must not be NULL");
  CHECK_THROWS_WITH(apply_mel_filterbank(&sample, 1, 1, nullptr, 1), "filterbank must not be NULL");
  CHECK_THROWS_WITH(apply_mel_filterbank(&sample, 0, 1, &sample, 1),
                    "n_bins must be an integer in [1, 2147483647]");
  CHECK_THROWS_WITH(apply_mel_filterbank(&sample, 1, 0, &sample, 1),
                    "n_frames must be an integer in [1, 2147483647]");
  CHECK_THROWS_WITH(apply_mel_filterbank(&sample, 1, 1, &sample, 0),
                    "n_mels must be an integer in [1, 2147483647]");
}

TEST_CASE("create_mel_filterbank refuses a band count whose extent wraps", "[mel]") {
  const int sr = 22050;
  const int n_fft = 2048;
  MelFilterConfig config;

  // The ceiling itself, and one band past it.
  config.n_mels = kMaxMelBands;
  REQUIRE_NOTHROW(create_mel_filterbank(sr, n_fft, config));
  config.n_mels = kMaxMelBands + 1;
  REQUIRE_THROWS_AS(create_mel_filterbank(sr, n_fft, config), SonareException);

  // 4190212 * (2048 / 2 + 1) is 4294967300, which truncates to 4 in an int: the
  // buffer came out four floats long while the filling loop still wrote every
  // band. Named here so the case fails for the arithmetic rather than for
  // being merely large.
  config.n_mels = 4190212;
  REQUIRE(static_cast<std::uint32_t>(static_cast<size_t>(config.n_mels) * (n_fft / 2 + 1)) == 4u);
  REQUIRE_THROWS_AS(create_mel_filterbank(sr, n_fft, config), SonareException);
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

TEST_CASE("create_mel_filterbank refuses an upper edge above Nyquist", "[mel][domain-mel]") {
  constexpr int sr = 22050;
  constexpr int n_fft = 2048;
  constexpr float nyquist = sr / 2.0f;

  MelFilterConfig config;
  config.n_mels = 40;
  config.fmax = nyquist + 1.0f;
  CHECK_THROWS_WITH(create_mel_filterbank(sr, n_fft, config),
                    "fmax must be finite and at most 11025");
  CHECK_THROWS_WITH(get_mel_filterbank_cached(sr, n_fft, config),
                    "fmax must be finite and at most 11025");

  config.fmax = nyquist;
  const auto supported = create_mel_filterbank(sr, n_fft, config);
  CHECK(supported.size() == static_cast<size_t>(config.n_mels * (n_fft / 2 + 1)));
  config.fmax = 0.0f;
  CHECK(create_mel_filterbank(sr, n_fft, config) == supported);

  config.fmax = static_cast<float>(std::numeric_limits<int>::max() / 2.0);
  CHECK_THROWS_AS(create_mel_filterbank(std::numeric_limits<int>::max(), n_fft, config),
                  SonareException);
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

  auto a = get_mel_filterbank_cached(sr, n_fft, config);
  auto b = get_mel_filterbank_cached(sr, n_fft, config);
  REQUIRE(a->data() == b->data());
  REQUIRE(a->size() == static_cast<size_t>(config.n_mels * (n_fft / 2 + 1)));

  // The cached values must match a freshly built filterbank.
  std::vector<float> fresh = create_mel_filterbank(sr, n_fft, config);
  REQUIRE(a->size() == fresh.size());
  for (size_t i = 0; i < fresh.size(); ++i) {
    REQUIRE_THAT((*a)[i], WithinAbs(fresh[i], 1e-6f));
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

  auto a = get_mel_filterbank_cached(sr, n_fft, config_default);
  auto b = get_mel_filterbank_cached(sr, n_fft, config_explicit);
  REQUIRE(a->data() == b->data());
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

  auto a = get_mel_filterbank_cached(sr, n_fft, c1);
  auto b = get_mel_filterbank_cached(sr, n_fft, c2);
  auto c = get_mel_filterbank_cached(sr, n_fft, c3);

  REQUIRE(a->data() != b->data());
  REQUIRE(a->data() != c->data());
  REQUIRE(b->data() != c->data());
  REQUIRE(a->size() != b->size());  // different n_mels → different shape
}

TEST_CASE("get_mel_filterbank_cached evicts oldest entries past capacity", "[mel][cache]") {
  // The implementation caps the cache at a small constant (kMaxMelCacheSize =
  // 8 at the time of writing). Insert more than that and verify the oldest
  // entry is evicted (a re-fetch builds a new filterbank). We do not pin the
  // exact capacity from the test — we only require eviction within a
  // reasonable upper bound so the test stays robust if the cap changes.
  //
  // The handle is held for the whole case, not dropped after reading data().
  // Dropping it lets eviction free the buffer, and an allocator is free to
  // hand the same address back for the rebuilt filterbank of the same size, so
  // comparing against the released address fails intermittently. Holding it
  // keeps the original allocation live, so a rebuilt entry cannot share its
  // address and the comparison is decided by the cache, not by malloc. This is
  // also the documented contract: the handle outlives eviction.
  int sr = 22050;
  int n_fft = 1024;

  MelFilterConfig first;
  first.n_mels = 16;
  const auto first_handle = get_mel_filterbank_cached(sr, n_fft, first);
  const void* first_ptr = first_handle->data();

  constexpr int kPressure = 32;
  for (int i = 0; i < kPressure; ++i) {
    MelFilterConfig c;
    c.n_mels = 17 + i;
    // Touch every distinct key once so they enter the LRU queue ahead of
    // `first` (which has not been re-touched since).
    (void)get_mel_filterbank_cached(sr, n_fft, c);
  }

  // After kPressure unique inserts the original entry must have been evicted,
  // so this call rebuilds it into fresh storage.
  const auto rebuilt_handle = get_mel_filterbank_cached(sr, n_fft, first);
  REQUIRE(rebuilt_handle->data() != first_ptr);
  // The evicted handle still describes the same filterbank it was built with.
  REQUIRE(*rebuilt_handle == *first_handle);
}

TEST_CASE("get_mel_filterbank_cached promotes on hit (true LRU)", "[mel][cache]") {
  // Verify the cache implements *true* LRU rather than plain FIFO: a key that
  // is re-touched after insertion must survive an eviction wave that would
  // otherwise reach it. We:
  //   1) insert key A,
  //   2) fill the cache to capacity with B..H (8 distinct entries including A),
  //   3) re-fetch A (must move A to MRU),
  //   4) insert one more distinct key (E_new) → eviction triggers,
  //   5) re-fetch A — its pointer must be unchanged (still cached).
  // If the cache were FIFO it would evict A in step 4.
  int sr = 22050;
  int n_fft = 1024;

  // Step 1: insert A. The handle is held for the whole case so A's storage
  // cannot be freed and recycled: without it, an incorrectly evicted A could be
  // rebuilt at the same address and the pointer comparisons below would pass on
  // a cache that had actually dropped it.
  MelFilterConfig a;
  a.n_mels = 16;
  const auto a_handle = get_mel_filterbank_cached(sr, n_fft, a);
  const void* a_ptr = a_handle->data();

  // Step 2: fill the remaining cache slots with 7 more distinct keys.
  // kMaxMelCacheSize is 8 today; we touch exactly cap-1 entries so A is the
  // oldest in insertion order but the cache is full.
  constexpr int kFill = 7;
  for (int i = 0; i < kFill; ++i) {
    MelFilterConfig c;
    c.n_mels = 17 + i;
    (void)get_mel_filterbank_cached(sr, n_fft, c);
  }

  // Step 3: re-touch A → splice to front of LRU.
  const void* a_ptr_promoted = get_mel_filterbank_cached(sr, n_fft, a)->data();
  REQUIRE(a_ptr_promoted == a_ptr);  // still the same storage

  // Step 4: insert one new key. Under FIFO this would evict A; under true LRU
  // it evicts whichever of the kFill keys is now the oldest (n_mels == 17).
  MelFilterConfig new_key;
  new_key.n_mels = 17 + kFill;  // distinct from everything inserted so far
  (void)get_mel_filterbank_cached(sr, n_fft, new_key);

  // Step 5: A must still be cached → same pointer.
  const void* a_ptr_after = get_mel_filterbank_cached(sr, n_fft, a)->data();
  REQUIRE(a_ptr_after == a_ptr);
}

TEST_CASE("mel filters refuse n_mels by its name", "[mel]") {
  CHECK_THROWS_WITH(mel_frequency_points(0, 0.0f, 8000.0f, false),
                    "n_points must be an integer in [1, 2147483647]");
  MelFilterConfig config;
  config.n_mels = 0;
  CHECK_THROWS_WITH(create_mel_filterbank(22050, 2048, config),
                    "n_mels must be an integer in [1, 4096]");
  config.n_mels = kMaxMelBands + 1;
  CHECK_THROWS_WITH(create_mel_filterbank(22050, 2048, config),
                    "n_mels must be an integer in [1, 4096]");
}

TEST_CASE("create_mel_filterbank refuses an empty band by naming fmax and fmin", "[mel]") {
  MelFilterConfig config;
  config.fmin = 4000.0f;
  config.fmax = 2000.0f;
  try {
    (void)create_mel_filterbank(22050, 2048, config);
    FAIL("an fmax below fmin was accepted");
  } catch (const SonareParameterRefusal& e) {
    CHECK(std::string(e.what()) == "fmax must be greater than fmin");
    CHECK(e.subjects() == std::vector<std::string>{"fmax", "fmin"});
  }
}

TEST_CASE("mel_frequency_points refuses an empty range by naming fmax and fmin", "[mel]") {
  try {
    (void)mel_frequency_points(8, 4000.0f, 4000.0f, false);
    FAIL("an fmax equal to fmin was accepted");
  } catch (const SonareParameterRefusal& e) {
    CHECK(std::string(e.what()) == "fmax must be greater than fmin");
    CHECK(e.subjects() == std::vector<std::string>{"fmax", "fmin"});
  }
}
