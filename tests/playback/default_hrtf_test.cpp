#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>

#include "playback/hrtf_set.h"

using namespace sonare::playback;

TEST_CASE("default HRTF set loads and its horizontal ITD grows from 0 to 90 degrees",
          "[playback][hrtf][default]") {
  std::unique_ptr<HrtfSet> set;
  REQUIRE_NOTHROW(set = std::make_unique<HrtfSet>(HrtfSet::builtin_default()));
  REQUIRE(set->azimuth_count() > 0);
  REQUIRE(set->elevation_step_deg() > 0.0f);

  const int el =
      static_cast<int>(std::lround((0.0f - set->elevation_min_deg()) / set->elevation_step_deg()));
  REQUIRE(el >= 0);
  REQUIRE(el < set->elevation_count());
  const int last_az = static_cast<int>(std::lround(90.0f / set->azimuth_step_deg()));
  for (int az = 1; az <= last_az; ++az) {
    CHECK(set->itd_samples(el, az) > set->itd_samples(el, az - 1));
  }
}
