/// @file formant_warp_test.cpp
/// @brief Tests for the offline LPC formant warp.

#include "effects/formant_warp.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

namespace {

sonare::Audio voiced_tone(int seconds, int sample_rate) {
  std::vector<float> samples(static_cast<size_t>(seconds) * static_cast<size_t>(sample_rate));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = 0.3f * std::sin(sonare::constants::kTwoPi * 180.0f * t) +
                 0.1f * std::sin(sonare::constants::kTwoPi * 900.0f * t);
  }
  return sonare::Audio::from_vector(std::move(samples), sample_rate);
}

}  // namespace

TEST_CASE("FormantWarp allocations do not grow with the analysis frame count",
          "[effects][formant]") {
  // Every hop runs an LPC analysis; its plan and scratch must be built once, so
  // three times the frames may not cost more allocations than one time.
  sonare::FormantWarpConfig config;
  config.factor = 1.2f;
  const sonare::FormantWarp warp(config);
  const sonare::Audio short_input = voiced_tone(1, 48000);
  const sonare::Audio long_input = voiced_tone(3, 48000);

  auto allocations = [&](const sonare::Audio& input) {
    sonare::test::AllocationGuard guard;
    const sonare::Audio output = warp.process(input);
    const size_t count = guard.count();
    REQUIRE(output.size() == input.size());
    return count;
  };
  const size_t short_count = allocations(short_input);
  const size_t long_count = allocations(long_input);
  CAPTURE(short_count, long_count);
  CHECK(long_count == short_count);
}
