/// @file stereo_delay_feedback_sign_test.cpp
/// @brief The stereo delay's feedback carries its sign into the loop.
///
/// A negative loop gain inverts every pass, so the echoes alternate in polarity
/// and mirror the positive loop's echo for echo. Read off the impulse response
/// rather than off the stored field, since a clamp at zero leaves a stored
/// negative value looking fine while the loop it names is open.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "effects/delay/stereo_delay.h"

namespace {

using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;

// Ten whole samples of delay, so every echo lands on one sample.
constexpr double kSampleRate = 1000.0;
constexpr float kDelayMs = 10.0f;
constexpr int kLength = 64;

StereoDelayConfig looped(float feedback) {
  StereoDelayConfig config;
  config.delay_time_l_ms = kDelayMs;
  config.delay_time_r_ms = kDelayMs;
  config.feedback = feedback;
  config.ping_pong = 0.0f;
  config.dry_wet = 1.0f;
  return config;
}

/// The left channel's response to an impulse on both channels.
std::vector<float> impulse_response(float feedback) {
  StereoDelay delay(looped(feedback));
  delay.prepare(kSampleRate, kLength);
  std::vector<float> left(kLength, 0.0f);
  std::vector<float> right(kLength, 0.0f);
  left[0] = right[0] = 1.0f;
  float* channels[2] = {left.data(), right.data()};
  delay.process(channels, 2, kLength);
  return left;
}

/// The echoes in the order they land, found rather than assumed from the delay
/// time: the loop's lap is the line plus the sample its feedback cell holds.
std::vector<float> echoes(const std::vector<float>& response) {
  std::vector<float> found;
  for (std::size_t i = 1; i < response.size(); ++i) {
    if (std::fabs(response[i]) > 1e-6f) found.push_back(response[i]);
  }
  return found;
}

}  // namespace

TEST_CASE("a negative stereo-delay feedback inverts every pass", "[effects][delay][stereo]") {
  constexpr float kGain = 0.5f;
  const std::vector<float> upright = echoes(impulse_response(kGain));
  const std::vector<float> inverted = echoes(impulse_response(-kGain));
  REQUIRE(upright.size() >= 3);
  REQUIRE(inverted.size() == upright.size());

  // Echo k carries gain^(k-1): the first is the line itself, whatever the sign.
  for (std::size_t k = 0; k < 3; ++k) {
    const float expected = std::pow(-kGain, static_cast<float>(k));
    INFO("echo " << k + 1);
    CHECK(std::fabs(inverted[k] - expected) <= 1e-5f);
    CHECK(std::fabs(inverted[k] - (k % 2 == 0 ? upright[k] : -upright[k])) <= 1e-5f);
  }
}

TEST_CASE("the stereo-delay feedback clamps symmetrically on every path",
          "[effects][delay][stereo]") {
  StereoDelay built(looped(-2.0f));
  CHECK(built.config().feedback == -0.95f);

  StereoDelay automated(looped(0.0f));
  REQUIRE(automated.set_parameter(2, -2.0f));
  CHECK(automated.config().feedback == -0.95f);
  REQUIRE(automated.set_parameter(2, 2.0f));
  CHECK(automated.config().feedback == 0.95f);
}

TEST_CASE("an inverted loop rings as long as an upright one", "[effects][delay][stereo]") {
  StereoDelay upright(looped(0.5f));
  StereoDelay inverted(looped(-0.5f));
  upright.prepare(48000.0, 512);
  inverted.prepare(48000.0, 512);
  REQUIRE(inverted.tail_samples() == upright.tail_samples());
  REQUIRE(inverted.tail_samples() > static_cast<int>(0.010 * 48000.0));
}
