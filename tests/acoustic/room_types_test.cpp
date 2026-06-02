#include "acoustic/room_types.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "analysis/acoustic_analyzer.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("acoustic scaffold: room geometry helpers", "[acoustic][room_types]") {
  const sonare::RoomDimensions dims{4.0f, 3.0f, 2.5f};

  SECTION("volume is the product of the three extents") {
    REQUIRE_THAT(sonare::acoustic::room_volume(dims), WithinAbs(30.0f, 1e-4f));
  }

  SECTION("surface area sums the three wall pairs") {
    // 2*(4*3 + 4*2.5 + 3*2.5) = 2*(12 + 10 + 7.5) = 59
    REQUIRE_THAT(sonare::acoustic::room_surface_area(dims), WithinAbs(59.0f, 1e-4f));
  }
}

TEST_CASE("acoustic scaffold: octave-band count tracks AcousticConfig", "[acoustic][room_types]") {
  // The synthesis material bands must line up with the analyzer's band split
  // so estimate->synthesize round-trips without band resampling.
  REQUIRE(sonare::acoustic::kDefaultOctaveBands == sonare::AcousticConfig{}.n_octave_bands);
}
