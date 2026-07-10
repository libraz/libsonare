/// @file numeric_overflow_guard_test.cpp
/// @brief Numeric-overflow hardening regression tests for core utilities.
/// @details Covers guards against signed-shift overflow in next_power_of_2(int)
///          (would spin forever) and undefined float->size_t casts in
///          Audio::slice for non-finite / out-of-range time bounds. The
///          flattened-index promotions in pcen.cpp / harmonic.cpp are guarded by
///          static reasoning (size_t accumulation) rather than a unit test, as
///          triggering int overflow there requires multi-gigabyte spectrograms.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "core/audio.h"
#include "util/math_utils.h"

using namespace sonare;

TEST_CASE("next_power_of_2(int) saturates for huge inputs without hanging",
          "[math_utils][overflow]") {
  // In-range behavior is preserved.
  REQUIRE(next_power_of_2(1) == 1);
  REQUIRE(next_power_of_2(3) == 4);
  REQUIRE(next_power_of_2(1024) == 1024);
  REQUIRE(next_power_of_2(1025) == 2048);
  REQUIRE(next_power_of_2(0) == 1);
  REQUIRE(next_power_of_2(-5) == 1);

  // An input just above the largest representable power of two must terminate
  // (previously the doubling overflowed to negative and looped forever) and
  // saturate at the cap rather than returning a negative value.
  constexpr int kCap = 1 << 30;
  const int result = next_power_of_2((1 << 30) + 1);
  REQUIRE(result == kCap);
  REQUIRE(result > 0);

  // The maximum valid int must not hang either.
  const int at_max = next_power_of_2(std::numeric_limits<int>::max());
  REQUIRE(at_max == kCap);
  REQUIRE(at_max > 0);
}

TEST_CASE("Audio::slice clamps out-of-range and non-finite time bounds", "[audio][overflow]") {
  std::vector<float> samples(1000, 0.25f);
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), 1000);

  SECTION("valid slice") {
    Audio s = audio.slice(0.1f, 0.5f);
    REQUIRE(s.size() == 400);
  }

  SECTION("start beyond end of audio yields empty slice, no UB") {
    Audio s = audio.slice(100.0f, 200.0f);
    REQUIRE(s.empty());
  }

  SECTION("end_time far past buffer clamps to length") {
    Audio s = audio.slice(0.0f, 1e30f);
    REQUIRE(s.size() == audio.size());
  }

  SECTION("non-finite start_time yields a well-defined slice, no UB") {
    const float inf = std::numeric_limits<float>::infinity();
    // A non-finite start is treated as 0 rather than an undefined
    // float->size_t cast; the resulting slice is well-defined and bounded.
    Audio s = audio.slice(inf, 0.5f);
    REQUIRE(s.size() <= audio.size());
    REQUIRE(s.size() == 500);
  }

  SECTION("non-finite end_time is rejected/clamped, no UB") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Audio s = audio.slice(0.0f, nan);
    // NaN end clamps to 0, so start >= end yields an empty slice.
    REQUIRE(s.empty());
  }

  SECTION("negative start_time clamps to 0") {
    Audio s = audio.slice(-5.0f, 0.5f);
    REQUIRE(s.size() == 500);
  }
}
