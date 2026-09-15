/// @file zero_crossings_test.cpp
/// @brief Unit tests for feature/spectral zero_crossings (raw indices).

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>

#include "feature/spectral.h"
#include "util/exception.h"

using namespace sonare;

TEST_CASE("zero_crossings finds simple sign flips", "[util][zero_crossings]") {
  std::vector<float> y{1.0f, -1.0f, 1.0f, -1.0f};
  auto z = zero_crossings(y);
  // pad=True: index 0 always included; then 1, 2, 3 all flip.
  REQUIRE(z.size() == 4);
  REQUIRE(z[0] == 0);
  REQUIRE(z[1] == 1);
  REQUIRE(z[2] == 2);
  REQUIRE(z[3] == 3);
}

TEST_CASE("zero_crossings honours pad=false", "[util][zero_crossings]") {
  std::vector<float> y{1.0f, -1.0f, 1.0f};
  auto z = zero_crossings(y, /*threshold=*/1e-10f, /*ref_magnitude=*/false,
                          /*pad=*/false, /*zero_pos=*/true);
  REQUIRE(z.size() == 2);
  REQUIRE(z[0] == 1);
  REQUIRE(z[1] == 2);
}

TEST_CASE("zero_crossings clips small values to zero with threshold", "[util][zero_crossings]") {
  // Values below threshold are treated as +0 (signbit false).
  std::vector<float> y{0.5f, 1e-12f, -0.5f, 1e-12f};
  auto z = zero_crossings(y, /*threshold=*/1e-6f, /*ref_magnitude=*/false,
                          /*pad=*/false, /*zero_pos=*/true);
  // y -> [+, +, -, +]; crossings at i=2 and i=3.
  REQUIRE(z.size() == 2);
  REQUIRE(z[0] == 2);
  REQUIRE(z[1] == 3);
}

TEST_CASE("zero_crossings on empty input", "[util][zero_crossings][edge]") {
  std::vector<float> y;
  auto z = zero_crossings(y);
  REQUIRE(z.empty());
}

TEST_CASE("zero_crossings rejects negative threshold", "[util][zero_crossings][edge]") {
  std::vector<float> y{1.0f, -1.0f};
  REQUIRE_THROWS_AS(zero_crossings(y, -1.0f), SonareException);
}

TEST_CASE("zero_crossings rejects a non-finite threshold", "[util][zero_crossings][edge]") {
  // +Infinity is the case a bare threshold >= 0 admits, and it fails silently rather
  // than loudly: every finite sample falls inside the band, so the scan reports no
  // crossings at all and returns an ordinary empty result.
  std::vector<float> y{1.0f, -1.0f};
  const float inf = std::numeric_limits<float>::infinity();

  REQUIRE_THROWS_AS(zero_crossings(y, inf), SonareException);
  REQUIRE_THROWS_AS(zero_crossings(y, -inf), SonareException);
  REQUIRE_THROWS_AS(zero_crossings(y, std::numeric_limits<float>::quiet_NaN()), SonareException);
}
