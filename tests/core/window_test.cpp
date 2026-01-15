/// @file window_test.cpp
/// @brief Tests for window functions.

#include "core/window.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("hann_window", "[window]") {
  auto win = hann_window(4);

  REQUIRE(win.size() == 4);
  REQUIRE_THAT(win[0], WithinAbs(0.0f, 1e-6f));
  REQUIRE_THAT(win[1], WithinAbs(0.75f, 1e-6f));
  REQUIRE_THAT(win[2], WithinAbs(0.75f, 1e-6f));
  REQUIRE_THAT(win[3], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("hamming_window", "[window]") {
  auto win = hamming_window(4);

  REQUIRE(win.size() == 4);
  // hamming(0) = 0.54 - 0.46 = 0.08
  REQUIRE_THAT(win[0], WithinAbs(0.08f, 1e-6f));
}

TEST_CASE("rectangular_window", "[window]") {
  auto win = rectangular_window(100);

  REQUIRE(win.size() == 100);
  for (float v : win) {
    REQUIRE_THAT(v, WithinAbs(1.0f, 1e-6f));
  }
}

TEST_CASE("window edge cases", "[window]") {
  SECTION("length 0") {
    auto hann = hann_window(0);
    auto hamming = hamming_window(0);
    auto blackman = blackman_window(0);
    auto rect = rectangular_window(0);

    REQUIRE(hann.empty());
    REQUIRE(hamming.empty());
    REQUIRE(blackman.empty());
    REQUIRE(rect.empty());
  }

  SECTION("length 1") {
    auto hann = hann_window(1);
    auto hamming = hamming_window(1);
    auto blackman = blackman_window(1);
    auto rect = rectangular_window(1);

    REQUIRE(hann.size() == 1);
    REQUIRE(hamming.size() == 1);
    REQUIRE(blackman.size() == 1);
    REQUIRE(rect.size() == 1);

    // All windows should return 1.0 for length 1
    REQUIRE_THAT(hann[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(hamming[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(blackman[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(rect[0], WithinAbs(1.0f, 1e-6f));
  }

  SECTION("length 2") {
    auto hann = hann_window(2);
    auto hamming = hamming_window(2);
    auto blackman = blackman_window(2);

    REQUIRE(hann.size() == 2);
    REQUIRE(hamming.size() == 2);
    REQUIRE(blackman.size() == 2);

    // Verify no divide by zero (endpoints should be valid)
    REQUIRE(std::isfinite(hann[0]));
    REQUIRE(std::isfinite(hann[1]));
    REQUIRE(std::isfinite(hamming[0]));
    REQUIRE(std::isfinite(hamming[1]));
    REQUIRE(std::isfinite(blackman[0]));
    REQUIRE(std::isfinite(blackman[1]));
  }
}
