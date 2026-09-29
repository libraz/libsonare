/// @file stereo_delay_taps_test.cpp
/// @brief The stereo delay's feed-forward taps, polarity, modulation, glide, cross modes and
/// mix law.
///
/// Read at 1 kHz so that one millisecond is one sample and an echo lands on a whole index.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "effects/delay/stereo_delay.h"

namespace {

using Catch::Approx;
using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;
using sonare::effects::delay::StereoDelayCrossMode;
using sonare::effects::common::MixLaw;

constexpr double kRate = 1000.0;
constexpr int kLength = 400;
constexpr double kTwoPi = 6.283185307179586;

struct Stereo {
  std::vector<float> left;
  std::vector<float> right;
};

/// Fully wet, no loop, the two lines set aside: only what a test switches on is heard.
StereoDelayConfig quiet() {
  StereoDelayConfig config;
  config.delay_time_l_ms = 0.0f;
  config.delay_time_r_ms = 0.0f;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;
  config.tap1_level_db = -120.0f;
  config.tap2_level_db = -120.0f;
  return config;
}

/// An impulse at sample `at` on the channels named by the two amplitudes.
Stereo run(const StereoDelayConfig& config, float in_l = 1.0f, float in_r = 1.0f, int at = 0,
           int length = kLength) {
  StereoDelay delay(config);
  delay.prepare(kRate, length);
  Stereo io{std::vector<float>(static_cast<std::size_t>(length), 0.0f),
            std::vector<float>(static_cast<std::size_t>(length), 0.0f)};
  io.left[static_cast<std::size_t>(at)] = in_l;
  io.right[static_cast<std::size_t>(at)] = in_r;
  float* channels[2] = {io.left.data(), io.right.data()};
  delay.process(channels, 2, length);
  return io;
}

float at(const std::vector<float>& v, int i) { return v[static_cast<std::size_t>(i)]; }

double centroid(const std::vector<float>& v, int lo, int hi) {
  double weight = 0.0;
  double moment = 0.0;
  for (int i = lo; i < hi; ++i) {
    const double h = std::fabs(static_cast<double>(at(v, i)));
    weight += h;
    moment += h * i;
  }
  return weight > 0.0 ? moment / weight : 0.0;
}

float peak_index(const std::vector<float>& v) {
  int best = 0;
  for (int i = 1; i < static_cast<int>(v.size()); ++i) {
    if (std::fabs(at(v, i)) > std::fabs(at(v, best))) best = i;
  }
  return static_cast<float>(best);
}

}  // namespace

TEST_CASE("taps 3 and 4 arrive at their times, levels and pans and vanish at zero",
          "[stereo-delay-taps]") {
  StereoDelayConfig config = quiet();
  config.tap3_ms = 20.0f;
  config.tap3_level_db = -6.0f;
  config.tap3_pan = -1.0f;
  config.tap4_ms = 40.0f;
  config.tap4_pan = 1.0f;
  Stereo out = run(config);
  const float minus_6_db = std::pow(10.0f, -6.0f / 20.0f);
  CHECK(at(out.left, 20) == Approx(minus_6_db).margin(1e-5));
  CHECK(at(out.right, 20) == Approx(0.0f).margin(1e-5));
  CHECK(at(out.right, 40) == Approx(1.0f).margin(1e-5));
  CHECK(at(out.left, 40) == Approx(0.0f).margin(1e-5));
  CHECK(at(out.left, 19) == Approx(0.0f).margin(1e-5));

  SECTION("a centred tap is constant power") {
    config.tap3_pan = 0.0f;
    out = run(config);
    CHECK(at(out.left, 20) == Approx(minus_6_db * std::sqrt(0.5f)).margin(1e-5));
    CHECK(at(out.right, 20) == Approx(minus_6_db * std::sqrt(0.5f)).margin(1e-5));
  }

  SECTION("a tap at zero milliseconds is gone") {
    config.tap3_ms = 0.0f;
    config.tap4_ms = 0.0f;
    out = run(config);
    for (int i = 0; i < kLength; ++i) {
      REQUIRE(std::fabs(at(out.left, i)) < 1e-5f);
      REQUIRE(std::fabs(at(out.right, i)) < 1e-5f);
    }
  }

  SECTION("a tap is a realtime parameter") {
    StereoDelay delay(quiet());
    delay.prepare(kRate, kLength);
    REQUIRE(delay.parameter_is_realtime_safe(6));
    REQUIRE(delay.set_parameter(6, 30.0f));
    REQUIRE(delay.config().tap3_ms == 30.0f);
    REQUIRE(delay.parameter_is_realtime_safe(21));
    REQUIRE_FALSE(delay.parameter_is_realtime_safe(22));
  }
}

TEST_CASE("taps 1 and 2 are the left and right lines and take their own level",
          "[stereo-delay-taps]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 10.0f;
  config.delay_time_r_ms = 30.0f;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;
  config.tap1_level_db = -6.0f;
  config.tap2_level_db = 6.0f;
  const Stereo out = run(config);
  CHECK(at(out.left, 10) == Approx(std::pow(10.0f, -6.0f / 20.0f)).margin(1e-5));
  CHECK(at(out.right, 30) == Approx(std::pow(10.0f, 6.0f / 20.0f)).margin(1e-5));
}

TEST_CASE("invert flips the polarity of the wet path per side", "[stereo-delay-taps]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 10.0f;
  config.delay_time_r_ms = 10.0f;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;
  config.tap3_ms = 20.0f;
  config.tap3_pan = 0.0f;
  const Stereo plain = run(config);
  config.invert_l = true;
  const Stereo flipped = run(config);
  for (const int i : {10, 20}) {
    CHECK(at(flipped.left, i) == Approx(-at(plain.left, i)).margin(1e-6));
    CHECK(at(flipped.right, i) == Approx(at(plain.right, i)).margin(1e-6));
  }
  CHECK(at(plain.left, 10) > 0.5f);
}

namespace {

/// Arrival index of an impulse written at `k` into a line of `base` samples modulated by
/// depth * sin(2 pi (n / kRate * rate - phase)): the n that solves n - d(n) = k.
double expected_arrival(int k, double base, double depth, double rate_hz, double phase) {
  double n = k + base;
  for (int i = 0; i < 50; ++i) {
    n = k + base + depth * std::sin(kTwoPi * (n / kRate * rate_hz - phase));
  }
  return n;
}

}  // namespace

TEST_CASE("modulation moves the delay at modRateHz by modDepthMs, the right side by modPhaseDeg",
          "[stereo-delay-taps]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 20.0f;
  config.delay_time_r_ms = 20.0f;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;
  config.mod_rate_hz = 1.0f;
  config.mod_depth_ms = 5.0f;
  config.mod_phase_deg = 90.0f;
  constexpr int kSamples = 1200;
  for (const int k : {150, 225, 700, 725}) {
    const Stereo out = run(config, 1.0f, 1.0f, k, kSamples);
    const double left = expected_arrival(k, 20.0, 5.0, 1.0, 0.0);
    const double right = expected_arrival(k, 20.0, 5.0, 1.0, 0.25);
    INFO("impulse at " << k);
    CHECK(centroid(out.left, k + 5, k + 40) == Approx(left).margin(0.3));
    CHECK(centroid(out.right, k + 5, k + 40) == Approx(right).margin(0.3));
  }

  SECTION("depth zero is no modulation") {
    config.mod_depth_ms = 0.0f;
    const Stereo out = run(config, 1.0f, 1.0f, 225, kSamples);
    CHECK(at(out.left, 245) == Approx(1.0f).margin(1e-6));
  }
}

TEST_CASE("glide slews a delay-time change over the time asked", "[stereo-delay-taps]") {
  auto arrival = [](float glide_ms) {
    StereoDelayConfig config;
    config.delay_time_l_ms = 10.0f;
    config.delay_time_r_ms = 10.0f;
    config.feedback = 0.0f;
    config.dry_wet = 1.0f;
    config.glide_ms = glide_ms;
    StereoDelay delay(config);
    delay.prepare(kRate, 600);
    delay.set_parameter(0, 90.0f);
    delay.set_parameter(1, 90.0f);
    std::vector<float> left(600, 0.0f);
    std::vector<float> right(600, 0.0f);
    left[30] = right[30] = 1.0f;
    float* channels[2] = {left.data(), right.data()};
    delay.process(channels, 2, 600);
    return peak_index(left);
  };
  CHECK(arrival(0.0f) > 100.0f);
  CHECK(arrival(200.0f) < 70.0f);
}

TEST_CASE("crossMode routes the loop as named", "[stereo-delay-taps]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 10.0f;
  config.delay_time_r_ms = 10.0f;
  config.feedback = 0.5f;
  config.ping_pong = 0.0f;
  config.dry_wet = 1.0f;

  // The loop lap is the line plus the sample the feedback cell holds.
  SECTION("normal keeps the echoes on their side") {
    const Stereo out = run(config, 1.0f, 0.0f);
    CHECK(at(out.left, 10) == Approx(1.0f).margin(1e-5));
    CHECK(at(out.left, 21) == Approx(0.5f).margin(1e-5));
    CHECK(at(out.right, 21) == Approx(0.0f).margin(1e-5));
  }

  SECTION("cross sends each echo to the other side and leaves the inputs where they are") {
    config.cross_mode = StereoDelayCrossMode::kCross;
    const Stereo out = run(config, 1.0f, 0.0f);
    CHECK(at(out.left, 10) == Approx(1.0f).margin(1e-5));
    CHECK(at(out.right, 10) == Approx(0.0f).margin(1e-5));
    CHECK(at(out.right, 21) == Approx(0.5f).margin(1e-5));
    CHECK(at(out.left, 21) == Approx(0.0f).margin(1e-5));
    CHECK(at(out.left, 32) == Approx(0.25f).margin(1e-5));
  }

  SECTION("ping-pong feeds the mono input into the left line only") {
    config.cross_mode = StereoDelayCrossMode::kPingPong;
    const Stereo out = run(config, 1.0f, 1.0f);
    CHECK(at(out.left, 10) == Approx(1.0f).margin(1e-5));
    CHECK(at(out.right, 10) == Approx(0.0f).margin(1e-5));
    CHECK(at(out.right, 21) == Approx(0.5f).margin(1e-5));
    CHECK(at(out.left, 21) == Approx(0.0f).margin(1e-5));
    CHECK(at(out.left, 32) == Approx(0.25f).margin(1e-5));
  }
}

TEST_CASE("mixLaw 1 keeps dry and wet as two independent ramps", "[stereo-delay-taps]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 10.0f;
  config.delay_time_r_ms = 10.0f;
  config.feedback = 0.0f;

  struct Row {
    float dry_wet;
    float dry;
    float wet;
  };
  // Each gain reaches unity at the halfway setting and only the far one falls away.
  for (const Row& row : {Row{0.25f, 1.0f, 0.5f}, Row{0.5f, 1.0f, 1.0f}, Row{0.75f, 0.5f, 1.0f}}) {
    config.dry_wet = row.dry_wet;
    config.mix_law = MixLaw::kTwoRamps;
    const Stereo ramps = run(config);
    INFO("dry_wet " << row.dry_wet);
    CHECK(at(ramps.left, 0) == Approx(row.dry).margin(1e-5));
    CHECK(at(ramps.left, 10) == Approx(row.wet).margin(1e-5));

    config.mix_law = MixLaw::kCrossfade;
    const Stereo fade = run(config);
    CHECK(at(fade.left, 0) == Approx(1.0f - row.dry_wet).margin(1e-5));
    CHECK(at(fade.left, 10) == Approx(row.dry_wet).margin(1e-5));
  }
}

TEST_CASE("switches and modes refuse a value that names none", "[stereo-delay-taps]") {
  StereoDelay delay(quiet());
  delay.prepare(kRate, kLength);
  for (const unsigned int id : {14u, 15u}) {
    REQUIRE(delay.set_parameter(id, 1.0f));
    REQUIRE(delay.set_parameter(id, 0.0f));
    for (const float bad : {0.5f, 2.0f, -1.0f}) REQUIRE_FALSE(delay.set_parameter(id, bad));
  }
  REQUIRE(delay.set_parameter(20, 2.0f));
  REQUIRE(delay.config().cross_mode == StereoDelayCrossMode::kCross);
  for (const float bad : {-1.0f, 1.5f, 3.0f}) {
    REQUIRE_FALSE(delay.set_parameter(20, bad));
    REQUIRE(delay.config().cross_mode == StereoDelayCrossMode::kCross);
  }
  REQUIRE(delay.set_parameter(21, 1.0f));
  REQUIRE(delay.config().mix_law == MixLaw::kTwoRamps);
  for (const float bad : {-1.0f, 0.5f, 2.0f}) REQUIRE_FALSE(delay.set_parameter(21, bad));
}
