#include "playback/hrtf_set.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <vector>

#include "playback/shrf_fixture.h"

using namespace sonare::playback;
using namespace sonare::playback::test;

TEST_CASE("HRTF set parses the synthetic SHRF fixture", "[playback][hrtf]") {
  const ShrfFixtureSpec spec;
  const std::vector<uint8_t> bytes = make_shrf_fixture(spec);
  std::unique_ptr<HrtfSet> set;
  REQUIRE_NOTHROW(set =
                      std::make_unique<HrtfSet>(HrtfSet::from_memory(bytes.data(), bytes.size())));
  CHECK(set->sample_rate() == spec.sample_rate);
  CHECK(set->taps() == spec.taps);
  CHECK(set->azimuth_count() == spec.n_az);
  CHECK(set->elevation_count() == spec.n_el);
  for (int az = 0; az < spec.n_az; ++az) {
    CHECK(std::abs(set->itd_samples(0, az) - fixture_itd_samples(spec, 0, az)) < 1e-6f);
  }
}
