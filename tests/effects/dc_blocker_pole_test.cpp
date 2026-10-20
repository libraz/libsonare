/// @file dc_blocker_pole_test.cpp
/// @brief The DC blocker's high-pass corner is set in hertz; prepare() derives the pole from it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <type_traits>

#include "effects/common/dc_blocker.h"
#include "util/constants.h"

using Catch::Matchers::WithinRel;
using sonare::effects::common::DcBlocker;

TEST_CASE("DcBlocker takes its corner in hertz, not as a constructor pole", "[fx][dc_blocker]") {
  // prepare() rewrites the pole from the corner, so a constructor pole would never survive it.
  CHECK_FALSE(std::is_constructible_v<DcBlocker, float>);
  CHECK(std::is_default_constructible_v<DcBlocker>);
}

TEST_CASE("DcBlocker derives its pole from the corner and the sample rate", "[fx][dc_blocker]") {
  constexpr double kRate = 48000.0;
  constexpr float kCornerHz = 40.0f;
  DcBlocker blocker;
  blocker.set_cutoff_hz(kCornerHz);
  blocker.prepare(kRate, 256);

  const double expected = std::exp(-sonare::constants::kTwoPiD * kCornerHz / kRate);
  CHECK_THAT(static_cast<double>(blocker.pole()), WithinRel(expected, 1e-5));
}
