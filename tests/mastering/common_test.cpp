#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "mastering/common/delay_line.h"
#include "mastering/common/envelope_follower.h"
#include "mastering/common/lookahead_buffer.h"
#include "mastering/common/oversampler.h"
#include "mastering/common/param_smoother.h"
#include "mastering/master.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::mastering::common;

TEST_CASE("Mastering umbrella header exposes representative modules", "[mastering]") {
  sonare::mastering::eq::ParametricEq eq;
  sonare::mastering::dynamics::Compressor compressor;
  sonare::mastering::maximizer::Maximizer maximizer;

  eq.prepare(48000.0, 16);
  compressor.prepare(48000.0, 16);
  maximizer.prepare(48000.0, 16);

  REQUIRE(eq.sample_rate() == 48000.0);
  REQUIRE(compressor.config().ratio == 2.0f);
  REQUIRE(maximizer.config().ceiling_db == -1.0f);
}

TEST_CASE("ParamSmoother reaches target immediately with zero time", "[mastering]") {
  ParamSmoother smoother(0.0f, 0.0f, 48000.0);

  smoother.set_target(1.0f);

  REQUIRE_THAT(smoother.process(), WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(smoother.current(), WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("ParamSmoother approaches target monotonically", "[mastering]") {
  ParamSmoother smoother(0.0f, 10.0f, 48000.0);

  smoother.set_target(1.0f);
  const float first = smoother.process();
  const float second = smoother.process();

  REQUIRE(first > 0.0f);
  REQUIRE(second > first);
  REQUIRE(second < 1.0f);
}

TEST_CASE("EnvelopeFollower follows attack and release", "[mastering]") {
  EnvelopeFollower follower;
  follower.prepare(1000.0, 0.0f, 100.0f);

  REQUIRE_THAT(follower.process(1.0f), WithinAbs(1.0f, 0.0001f));

  const float released = follower.process(0.0f);
  REQUIRE(released > 0.0f);
  REQUIRE(released < 1.0f);
}

TEST_CASE("DelayLine delays by configured samples", "[mastering]") {
  DelayLine delay;
  delay.prepare(2);

  REQUIRE_THAT(delay.process(1.0f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(delay.process(2.0f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(delay.process(3.0f), WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(delay.process(4.0f), WithinAbs(2.0f, 0.0001f));
}

TEST_CASE("DelayLine zero delay returns input immediately", "[mastering]") {
  DelayLine delay;
  delay.prepare(0);

  REQUIRE_THAT(delay.process(1.0f), WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(delay.process(-0.5f), WithinAbs(-0.5f, 0.0001f));
}

TEST_CASE("LookaheadBuffer exposes peak while delaying output", "[mastering]") {
  LookaheadBuffer buffer;
  buffer.prepare(2);

  REQUIRE_THAT(buffer.process(0.25f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(0.25f, 0.0001f));

  REQUIRE_THAT(buffer.process(-1.0f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(1.0f, 0.0001f));

  REQUIRE_THAT(buffer.process(0.5f), WithinAbs(0.25f, 0.0001f));
  REQUIRE_THAT(buffer.peak(), WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("Oversampler linearly interpolates intermediate samples", "[mastering]") {
  Oversampler oversampler(4);
  const std::vector<float> input = {0.0f, 1.0f, 0.0f};

  const auto upsampled = oversampler.upsample(input);

  REQUIRE(upsampled.size() == input.size() * 4);
  REQUIRE_THAT(upsampled[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(upsampled[1], WithinAbs(0.25f, 0.0001f));
  REQUIRE_THAT(upsampled[2], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(upsampled[3], WithinAbs(0.75f, 0.0001f));
  REQUIRE_THAT(upsampled[4], WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(upsampled[8], WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("Oversampler downsample recovers original samples after upsample", "[mastering]") {
  Oversampler oversampler(8);
  const std::vector<float> input = {-0.5f, 0.25f, 1.0f, -1.0f};

  const auto upsampled = oversampler.upsample(input);
  const auto downsampled = oversampler.downsample(upsampled);

  REQUIRE(downsampled.size() == input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    REQUIRE_THAT(downsampled[i], WithinAbs(input[i], 0.0001f));
  }
}

TEST_CASE("Oversampler supports only power of two mastering factors", "[mastering]") {
  Oversampler oversampler(2);

  oversampler.set_factor(4);
  REQUIRE(oversampler.factor() == 4);
  oversampler.set_factor(8);
  REQUIRE(oversampler.factor() == 8);
  REQUIRE_THROWS(oversampler.set_factor(3));
}
