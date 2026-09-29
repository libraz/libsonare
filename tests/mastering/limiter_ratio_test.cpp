/// @file limiter_ratio_test.cpp
/// @brief Limiter ratio and post gain, and the graphic EQ's shared Q.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "mastering/dynamics/limiter.h"
#include "mastering/eq/graphic_eq.h"

namespace {

using sonare::mastering::dynamics::Limiter;
using sonare::mastering::dynamics::LimiterConfig;
using sonare::mastering::eq::GraphicEq;

constexpr double kRate = 48000.0;
constexpr int kBlock = 9600;

/// Steady-state output level in dB of a constant 0 dBFS input.
float settled_db(Limiter& limiter) {
  limiter.prepare(kRate, kBlock);
  std::vector<float> left(kBlock, 1.0f);
  std::vector<float> right(kBlock, 1.0f);
  float* channels[2] = {left.data(), right.data()};
  limiter.process(channels, 2, kBlock);
  return 20.0f * std::log10(left.back());
}

}  // namespace

TEST_CASE("a finite limiter ratio follows the 1/ratio static curve above threshold",
          "[limiter-ratio]") {
  for (const float ratio : {1.5f, 4.0f, 20.0f}) {
    LimiterConfig config;
    config.threshold_db = -20.0f;
    config.ratio = ratio;
    Limiter limiter(config);
    // A 0 dB input sits 20 dB over the ceiling and leaves 20/ratio dB over it.
    REQUIRE(settled_db(limiter) == Catch::Approx(-20.0f + 20.0f / ratio).margin(0.01f));
  }
}

TEST_CASE("ratio 0 is a brick-wall at the threshold", "[limiter-ratio]") {
  LimiterConfig config;
  config.threshold_db = -20.0f;
  Limiter limiter(config);
  REQUIRE(settled_db(limiter) == Catch::Approx(-20.0f).margin(0.001f));
}

TEST_CASE("limiter post gain adds exactly its gain", "[limiter-ratio]") {
  LimiterConfig base;
  base.threshold_db = -20.0f;
  Limiter reference(base);
  const float without = settled_db(reference);
  base.post_gain_db = 6.0f;
  Limiter boosted(base);
  REQUIRE(settled_db(boosted) - without == Catch::Approx(6.0f).margin(0.001f));
}

TEST_CASE("limiter ratio and post gain are realtime parameters", "[limiter-ratio]") {
  LimiterConfig config;
  config.threshold_db = -20.0f;
  Limiter limiter(config);
  limiter.prepare(kRate, kBlock);
  REQUIRE(limiter.set_parameter(2, 4.0f));
  REQUIRE(limiter.set_parameter(3, -3.0f));
  std::vector<float> left(kBlock, 1.0f);
  std::vector<float> right(kBlock, 1.0f);
  float* channels[2] = {left.data(), right.data()};
  limiter.process(channels, 2, kBlock);
  REQUIRE(20.0f * std::log10(left.back()) == Catch::Approx(-15.0f - 3.0f).margin(0.01f));
  const auto descriptors = limiter.parameter_descriptors();
  REQUIRE(descriptors.size() == 4);
  REQUIRE(descriptors[2].id == 2);
  REQUIRE(descriptors[3].id == 3);
}

TEST_CASE("a limiter ratio between 0 and 1 is rejected", "[limiter-ratio]") {
  LimiterConfig config;
  config.ratio = 0.5f;
  REQUIRE_THROWS(Limiter(config));
}

TEST_CASE("a realtime limiter ratio or post gain outside the domain is refused",
          "[limiter-ratio]") {
  Limiter limiter(LimiterConfig{});
  limiter.prepare(kRate, kBlock);
  for (float bad : {0.5f, -1.0f, NAN, INFINITY}) {
    CAPTURE(bad);
    CHECK_FALSE(limiter.set_parameter(2, bad));
  }
  CHECK_FALSE(limiter.set_parameter(3, NAN));
  CHECK_FALSE(limiter.set_parameter(3, INFINITY));
  CHECK(limiter.set_parameter(2, 0.0f));
  CHECK(limiter.set_parameter(2, 1.0f));
}

TEST_CASE("the graphic EQ's shared Q narrows a band", "[graphic-q]") {
  auto peak_width = [](float q) {
    GraphicEq eq;
    eq.prepare(kRate, 4096);
    eq.set_gain_db(17, 12.0f);
    if (q > 0.0f) REQUIRE(eq.set_parameter(GraphicEq::kNumBands, q));
    std::vector<float> response(16384, 0.0f);
    response[0] = 1.0f;
    float* channels[1] = {response.data()};
    eq.process(channels, 1, static_cast<int>(response.size()));
    // Count bins within 3 dB of the peak, over the low half of the spectrum.
    const auto magnitude_db = [&](double hz) {
      std::complex<double> sum = 0.0;
      const double w = 2.0 * 3.14159265358979323846 * hz / kRate;
      for (size_t n = 0; n < response.size(); ++n) {
        sum += static_cast<double>(response[n]) * std::polar(1.0, -w * static_cast<double>(n));
      }
      return 20.0 * std::log10(std::abs(sum));
    };
    const double peak = magnitude_db(1000.0);
    double width = 0.0;
    for (double hz = 500.0; hz <= 2000.0; hz += 5.0) {
      if (magnitude_db(hz) > peak - 3.0103) width += 5.0;
    }
    return width;
  };
  const double wide = peak_width(1.0f);
  const double narrow = peak_width(8.0f);
  REQUIRE(narrow < wide / 4.0);
  // 0 keeps the gain-derived default (Q 7.5 at +12 dB).
  REQUIRE(peak_width(0.0f) == Catch::Approx(peak_width(GraphicEq::band_q_for_gain_db(12.0f))));
}
