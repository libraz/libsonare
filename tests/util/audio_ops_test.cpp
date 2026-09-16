/// @file audio_ops_test.cpp
/// @brief Unit tests for core/audio_ops (mu-law / autocorrelate / LPC).

#include "core/audio_ops.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "util/exception.h"
#include "util/lpc.h"

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

TEST_CASE("lpc deterministic order-1 matches reference", "[audio_ops][util]") {
  // Deterministic order-1 Burg LPC of {1,2,3,4}. With fwd = y[1:], bwd = y[:-1]:
  //   dot = 1*2 + 2*3 + 3*4 = 20
  //   den = (4+9+16) + (1+4+9) = 43
  //   a_1 = -2*dot/den = -40/43 = -0.93023256
  // Verified against librosa.lpc([1,2,3,4], order=1) -> [1, -0.93023256].
  // Locks in the index-offset refactor: the result must be unchanged.
  std::vector<float> y{1.0f, 2.0f, 3.0f, 4.0f};
  auto coeffs = lpc(y, 1);
  REQUIRE(coeffs.size() == 2);
  REQUIRE(coeffs[0] == 1.0f);
  REQUIRE_THAT(coeffs[1], WithinAbs(-0.93023256f, 1e-6f));
}

TEST_CASE("lpc delegates Burg coefficients to the shared util implementation",
          "[audio_ops][util]") {
  std::vector<float> y{0.1f, -0.4f, 0.7f, 0.2f, -0.3f, 0.5f, -0.2f, 0.1f};
  const int order = 3;

  const auto coeffs = lpc(y, order);
  const auto model = lpc_burg(y.data(), y.size(), order);

  REQUIRE(coeffs.size() == model.ar.size());
  for (size_t i = 0; i < coeffs.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(coeffs[i], WithinAbs(model.ar[i], 0.0f));
  }
  REQUIRE(model.variance >= 0.0f);
}

TEST_CASE("lpc_autocorrelation writes a reused result exactly as a fresh one",
          "[audio_ops][util]") {
  // A per-frame analysis loop reuses one LpcResult, so every field it carries
  // must be overwritten, not merged with what the previous frame left there.
  // Exact ==: the two overloads run the same arithmetic, and a partially
  // rewritten buffer differs by whole coefficients, not by an ulp.
  std::mt19937 rng(20260912u);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  LpcResult reused;
  for (int frame = 0; frame < 8; ++frame) {
    // Alternate the order so a stale coefficient past the new order survives in
    // the reused buffer if assign() ever stops covering it, and alternate the
    // frame between signal and silence so the early-return path is reused too.
    const int order = (frame % 2 == 0) ? 6 : 3;
    std::vector<float> frame_samples(32);
    for (float& sample : frame_samples) {
      sample = (frame % 4 == 3) ? 0.0f : dist(rng);
    }

    lpc_autocorrelation(frame_samples.data(), frame_samples.size(), order, &reused);
    const LpcResult fresh = lpc_autocorrelation(frame_samples.data(), frame_samples.size(), order);

    CAPTURE(frame, order);
    REQUIRE(reused.ar.size() == fresh.ar.size());
    for (size_t i = 0; i < fresh.ar.size(); ++i) {
      CAPTURE(i);
      CHECK(reused.ar[i] == fresh.ar[i]);
    }
    CHECK(reused.variance == fresh.variance);
  }
}

TEST_CASE("lpc_autocorrelation rejects a null result", "[audio_ops][util][edge]") {
  std::vector<float> y{0.1f, -0.4f, 0.7f, 0.2f};
  REQUIRE_THROWS_AS(lpc_autocorrelation(y.data(), y.size(), 2, nullptr), SonareException);
}

TEST_CASE("lpc rejects invalid order", "[audio_ops][util][edge]") {
  std::vector<float> y(8, 1.0f);
  REQUIRE_THROWS_AS(lpc(y, 0), SonareException);
}

TEST_CASE("raw LPC helpers accept valid empty null inputs", "[audio_ops][util][edge]") {
  LpcResult model;
  model.ar = {1.0f};

  REQUIRE(lpc_residual(nullptr, 0, model).empty());
  REQUIRE_THROWS_AS(lpc_residual(nullptr, 1, model), SonareException);

  // The gap fillers take the buffer directly, so an empty buffer is the only
  // null they accept and a gap outside the buffer is a parameter error.
  const ArInterpolateParams params;
  REQUIRE_NOTHROW(interpolate_gap(nullptr, 0, 0, 0));
  REQUIRE_NOTHROW(ar_interpolate_region(nullptr, 0, 0, 0, params));
  REQUIRE_THROWS_AS(interpolate_gap(nullptr, 1, 0, 1), SonareException);
  REQUIRE_THROWS_AS(ar_interpolate_region(nullptr, 1, 0, 1, params), SonareException);

  std::vector<float> samples(8, 0.25f);
  REQUIRE_THROWS_AS(interpolate_gap(samples.data(), samples.size(), 2, 9), SonareException);
  REQUIRE_THROWS_AS(ar_interpolate_region(samples.data(), samples.size(), 5, 2, params),
                    SonareException);
}
