#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "filters/chroma.h"

TEST_CASE("Pitch class conversion rejects frequencies without a finite pitch", "[chroma][edge]") {
  REQUIRE(sonare::hz_to_pitch_class(std::numeric_limits<float>::quiet_NaN()) == -1);
  REQUIRE(sonare::hz_to_pitch_class(std::numeric_limits<float>::infinity()) == -1);
  REQUIRE(sonare::hz_to_pitch_class(std::numeric_limits<float>::denorm_min()) == -1);
  REQUIRE(sonare::hz_to_pitch_class(440.0f) == 9);
}
