/// @file weighting_test.cpp
/// @brief Smoke tests for core/weighting.

#include "core/weighting.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("A_weighting is approximately 0 dB at 1000 Hz", "[util][weighting]") {
  // The A-weighting curve passes through 0 dB at 1 kHz by design.
  auto w = A_weighting({1000.0f});
  REQUIRE(w.size() == 1);
  REQUIRE_THAT(w[0], WithinAbs(0.0f, 1e-2f));
}

TEST_CASE("C_weighting is approximately 0 dB at 1000 Hz", "[util][weighting]") {
  auto w = C_weighting({1000.0f});
  REQUIRE(w.size() == 1);
  REQUIRE_THAT(w[0], WithinAbs(0.0f, 1e-2f));
}

TEST_CASE("frequency_weighting dispatches kind", "[util][weighting]") {
  std::vector<float> freqs{100.0f, 1000.0f, 5000.0f};
  auto a1 = A_weighting(freqs);
  auto a2 = frequency_weighting(freqs, "A");
  REQUIRE(a1.size() == a2.size());
  for (size_t i = 0; i < a1.size(); ++i) {
    REQUIRE_THAT(a1[i], WithinAbs(a2[i], 1e-5f));
  }

  // Z weighting (flat).
  auto z = frequency_weighting(freqs, "Z");
  REQUIRE(z.size() == freqs.size());
  for (float v : z) {
    REQUIRE_THAT(v, WithinAbs(0.0f, 1e-9f));
  }
}

TEST_CASE("frequency_weighting rejects unknown kind", "[util][weighting][edge]") {
  std::vector<float> freqs{100.0f};
  REQUIRE_THROWS_AS(frequency_weighting(freqs, "X"), SonareException);
}

TEST_CASE("min_db floor is honored", "[util][weighting]") {
  // 10 Hz produces a very negative A-weight; floor should clamp it.
  auto w = A_weighting({10.0f}, /*min_db=*/-20.0f);
  REQUIRE(w.size() == 1);
  REQUIRE(w[0] >= -20.0f - 1e-6f);
}
