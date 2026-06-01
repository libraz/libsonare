/// @file synthesis_test.cpp
/// @brief Smoke tests for core/synthesis.

#include "core/synthesis.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using sonare::constants::kTwoPi;

TEST_CASE("tone produces a sine of correct length and frequency", "[util][synthesis]") {
  const int sr = 22050;
  const float duration = 0.1f;
  Audio y = tone(440.0f, sr, duration);
  const size_t expected_n = static_cast<size_t>(duration * sr);
  REQUIRE(y.size() == expected_n);
  REQUIRE(y.sample_rate() == sr);

  // y[i] = sin(2pi*440*i/sr) at phi=0.
  for (size_t i = 0; i < 16; ++i) {
    float expected = std::sin(kTwoPi * 440.0f * static_cast<float>(i) / sr);
    REQUIRE_THAT(y[i], WithinAbs(expected, 1e-5f));
  }
}

TEST_CASE("tone with amplitude scales output", "[util][synthesis]") {
  Audio y = tone(440.0f, 22050, 0.01f, /*phi=*/0.0f, /*amplitude=*/0.5f);
  float max_abs = 0.0f;
  for (size_t i = 0; i < y.size(); ++i) {
    if (std::abs(y[i]) > max_abs) max_abs = std::abs(y[i]);
  }
  REQUIRE(max_abs <= 0.5f + 1e-5f);
  REQUIRE(max_abs > 0.4f);
}

TEST_CASE("chirp linear has growing instantaneous frequency", "[util][synthesis]") {
  Audio y = chirp(100.0f, 2000.0f, 22050, 0.5f, /*linear=*/true);
  REQUIRE(y.size() == static_cast<size_t>(0.5f * 22050));
  REQUIRE(y.sample_rate() == 22050);

  // Count zero crossings near start vs near end. End should have more.
  auto count_zc = [&](size_t start, size_t end) {
    int n = 0;
    for (size_t i = start + 1; i < end; ++i) {
      if ((y[i] >= 0.0f) != (y[i - 1] >= 0.0f)) ++n;
    }
    return n;
  };
  int zc_start = count_zc(0, 1000);
  int zc_end = count_zc(y.size() - 1000, y.size());
  REQUIRE(zc_end > zc_start);
}

TEST_CASE("chirp exponential requires positive endpoints", "[util][synthesis][edge]") {
  REQUIRE_THROWS_AS(chirp(0.0f, 1000.0f, 22050, 0.1f, /*linear=*/false), SonareException);
}

TEST_CASE("clicks places clicks at expected positions", "[util][synthesis]") {
  const int sr = 22050;
  std::vector<float> times{0.0f, 0.05f};
  Audio y = clicks(times, sr, /*length=*/0, /*frequency=*/1000.0f,
                   /*click_duration=*/0.02f);
  REQUIRE(!y.empty());

  // First click starts at position 0. Look for non-zero data near 0.
  bool found_early = false;
  for (size_t i = 0; i < 4; ++i) {
    if (std::abs(y[i]) > 1e-9f) {
      found_early = true;
      break;
    }
  }
  // First few samples may be near zero (sin(small_angle)*1.0). Check within
  // first 50 samples instead.
  if (!found_early) {
    for (size_t i = 0; i < 50; ++i) {
      if (std::abs(y[i]) > 1e-5f) {
        found_early = true;
        break;
      }
    }
  }
  REQUIRE(found_early);

  // Second click position: 0.05s -> sample 1102.
  const size_t second_pos = static_cast<size_t>(0.05f * sr);
  bool found_second = false;
  for (size_t i = second_pos; i < second_pos + 50 && i < y.size(); ++i) {
    if (std::abs(y[i]) > 1e-5f) {
      found_second = true;
      break;
    }
  }
  REQUIRE(found_second);
}

TEST_CASE("clicks rejects invalid params", "[util][synthesis][edge]") {
  REQUIRE_THROWS_AS(clicks({}, 22050, 0, 0.0f, 0.1f), SonareException);
  REQUIRE_THROWS_AS(clicks({}, 22050, 0, 1000.0f, 0.0f), SonareException);
  REQUIRE_THROWS_AS(clicks({}, 22050, -1, 1000.0f, 0.1f), SonareException);
}
