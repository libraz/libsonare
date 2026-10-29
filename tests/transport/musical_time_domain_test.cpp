#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

#include "transport/musical_time.h"

TEST_CASE("samples_to_ppq_duration rejects non-finite tempo and sample rates",
          "[transport][validation]") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();

  REQUIRE(sonare::transport::samples_to_ppq_duration(24000, infinity, 48000.0) == 0.0);
  REQUIRE(sonare::transport::samples_to_ppq_duration(24000, 120.0, infinity) == 0.0);
  REQUIRE(sonare::transport::samples_to_ppq_duration(24000, 120.0, nan) == 0.0);
  REQUIRE(sonare::transport::samples_to_ppq_duration(0, infinity, 48000.0) == 0.0);
  REQUIRE(sonare::transport::samples_to_ppq_duration(24000, 120.0, 48000.0) == 1.0);
}
