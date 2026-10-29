#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "midi/clock_sync.h"

TEST_CASE("Song position conversion handles nonfinite PPQ without narrowing it",
          "[midi][clock][domain]") {
  using sonare::midi::ppq_to_spp_beats;
  REQUIRE(ppq_to_spp_beats(std::numeric_limits<double>::quiet_NaN()) == 0);
  REQUIRE(ppq_to_spp_beats(-std::numeric_limits<double>::infinity()) == 0);
  REQUIRE(ppq_to_spp_beats(std::numeric_limits<double>::infinity()) == 0x3fff);
  REQUIRE(ppq_to_spp_beats(-1.0) == 0);
  REQUIRE(ppq_to_spp_beats(0.0) == 0);
  REQUIRE(ppq_to_spp_beats(1.49) == 5);
  REQUIRE(ppq_to_spp_beats(std::numeric_limits<double>::max()) == 0x3fff);
}
