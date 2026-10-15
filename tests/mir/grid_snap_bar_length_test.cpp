#include <catch2/catch_test_macros.hpp>

#include "mir/grid_snap.h"
#include "transport/tempo_map.h"

TEST_CASE("grid bar length is the transport bar length for every signature", "[mir]") {
  for (int numerator = 0; numerator <= 16; ++numerator) {
    for (int denominator = 0; denominator <= 16; ++denominator) {
      sonare::mir::SnapGrid grid;
      grid.time_sig = {numerator, denominator};
      INFO(numerator << "/" << denominator);
      CHECK(sonare::mir::bar_length_ppq(grid) == sonare::transport::bar_length_ppq(grid.time_sig));
    }
  }
}
