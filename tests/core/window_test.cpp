/// @file window_test.cpp
/// @brief Tests for window functions.

#include "core/window.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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
