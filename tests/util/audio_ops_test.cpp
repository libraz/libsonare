/// @file audio_ops_test.cpp
/// @brief Unit tests for core/audio_ops (mu-law / autocorrelate / LPC).

#include "core/audio_ops.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("mu_compress / mu_expand round-trip (no quantize)", "[audio_ops][util]") {
  std::vector<float> x{-1.0f, -0.5f, -0.1f, 0.0f, 0.1f, 0.5f, 1.0f};
  auto c = mu_compress(x, 255, /*quantize=*/false);
  auto r = mu_expand(c, 255, /*quantize=*/false);
  REQUIRE(r.size() == x.size());
  for (size_t i = 0; i < x.size(); ++i) {
    CAPTURE(i, x[i], r[i]);
    REQUIRE_THAT(r[i], WithinAbs(x[i], 1e-5f));
  }
}

TEST_CASE("mu_compress rejects out-of-range input", "[audio_ops][util][edge]") {
  std::vector<float> x{1.5f};
  REQUIRE_THROWS_AS(mu_compress(x, 255, false), SonareException);
}

TEST_CASE("mu_compress rejects non-positive mu", "[audio_ops][util][edge]") {
  std::vector<float> x{0.5f};
  REQUIRE_THROWS_AS(mu_compress(x, 0, false), SonareException);
}

TEST_CASE("autocorrelate[0] equals sum of squares", "[audio_ops][util]") {
  std::vector<float> y{0.5f, -1.0f, 0.25f, 0.75f};
  auto a = autocorrelate(y);
  REQUIRE(a.size() == y.size());
  float sumsq = 0.0f;
  for (float v : y) sumsq += v * v;
  REQUIRE_THAT(a[0], WithinRel(sumsq, 1e-4f));
}

TEST_CASE("autocorrelate respects max_size", "[audio_ops][util]") {
  std::vector<float> y(64, 1.0f);
  auto a = autocorrelate(y, 8);
  REQUIRE(a.size() == 8);
}

TEST_CASE("lpc on AR(2) recovers approximate coefficients", "[audio_ops][util]") {
  // Generate AR(2) process: y[n] = -a1*y[n-1] - a2*y[n-2] + eps[n].
  const int n = 1024;
  const float a1 = -1.2f;
  const float a2 = 0.5f;
  std::mt19937 rng(42);
  std::normal_distribution<float> dist(0.0f, 0.1f);
  std::vector<float> y(n, 0.0f);
  for (int i = 2; i < n; ++i) {
    y[i] = dist(rng) - a1 * y[i - 1] - a2 * y[i - 2];
  }
  auto coeffs = lpc(y, 2);
  REQUIRE(coeffs.size() == 3);
  REQUIRE(coeffs[0] == 1.0f);
  // Expect coefficients close to [1, a1, a2].
  REQUIRE_THAT(coeffs[1], WithinAbs(a1, 0.1f));
  REQUIRE_THAT(coeffs[2], WithinAbs(a2, 0.1f));
}

TEST_CASE("lpc rejects invalid order", "[audio_ops][util][edge]") {
  std::vector<float> y(8, 1.0f);
  REQUIRE_THROWS_AS(lpc(y, 0), SonareException);
}
