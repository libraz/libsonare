#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "effects/delay/stereo_delay.h"
#include "util/constants.h"

// ping_pong must be smoothed per-sample with the same exponential coefficient as
// feedback/dry_wet. Reading it once per block while the others smooth per-sample
// steps the feedback-tap crossfade weight and produces audible zipper noise.
//
// smoothed_ping_pong_ is private, so these tests observe its effect through the
// wet output. Asymmetric feedback state (signal on the left channel only) makes
// the ping_pong crossfade weight audible; a channel-symmetric input would leave
// the two feedback taps equal and hide the parameter entirely.
namespace {

using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;

constexpr double kSampleRate = 1000.0;
// 10 ms delay lines at the smoothing time scale keep the feedback taps distinct.
constexpr float kDelayMs = 10.0f;
constexpr float kFeedback = 0.5f;
constexpr float kDryWet = 0.5f;

// Drives the delay with a sine on the left channel and silence on the right so
// the two feedback taps diverge, then returns the left-channel wet output.
std::vector<float> run(StereoDelay& delay, int warmup, int measure) {
  std::vector<float> out;
  out.reserve(static_cast<std::size_t>(measure));
  std::array<float, 1> block_l{};
  std::array<float, 1> block_r{};
  float* channels[2] = {block_l.data(), block_r.data()};
  double phase = 0.0;
  const double step = sonare::constants::kTwoPiD * 60.0 / kSampleRate;
  auto tick = [&](std::vector<float>* sink) {
    block_l[0] = static_cast<float>(std::sin(phase));
    block_r[0] = 0.0f;
    phase += step;
    delay.process(channels, 2, 1);
    if (sink != nullptr) sink->push_back(block_l[0]);
  };
  for (int i = 0; i < warmup; ++i) tick(nullptr);
  for (int i = 0; i < measure; ++i) tick(&out);
  return out;
}

}  // namespace

TEST_CASE("StereoDelay ping_pong automation converges toward the target", "[fx]") {
  // Reference: constructed already at the target ping_pong, so its smoothed
  // value is settled at 1 after warmup.
  StereoDelay reference(StereoDelayConfig{kDelayMs, kDelayMs, kFeedback, 1.0f, kDryWet});
  reference.prepare(kSampleRate, 1);

  // Under test: constructed at ping_pong = 0, warmed up, then stepped to 1. The
  // smoothed weight must ramp toward the reference rather than jump.
  StereoDelay stepped(StereoDelayConfig{kDelayMs, kDelayMs, kFeedback, 0.0f, kDryWet});
  stepped.prepare(kSampleRate, 1);

  constexpr int kWarmup = 3000;
  constexpr int kMeasure = 2000;

  // Warm both to their respective steady states, keeping phase aligned by
  // discarding the warmup output.
  run(reference, kWarmup, 0);
  run(stepped, kWarmup, 0);

  // Step the parameter and measure both from the same phase.
  REQUIRE(stepped.set_parameter(3, 1.0f));
  const std::vector<float> ref_out = run(reference, 0, kMeasure);
  const std::vector<float> test_out = run(stepped, 0, kMeasure);
  REQUIRE(ref_out.size() == static_cast<std::size_t>(kMeasure));
  REQUIRE(test_out.size() == static_cast<std::size_t>(kMeasure));

  auto window_mean_abs_diff = [&](int from, int count) {
    double acc = 0.0;
    for (int i = from; i < from + count; ++i) {
      acc += std::abs(static_cast<double>(test_out[static_cast<std::size_t>(i)]) -
                      static_cast<double>(ref_out[static_cast<std::size_t>(i)]));
    }
    return acc / count;
  };

  // Right after the step the two delays differ; once the smoothed ping_pong
  // settles they must track each other closely.
  const double early = window_mean_abs_diff(0, 200);
  const double late = window_mean_abs_diff(kMeasure - 200, 200);
  REQUIRE(early > 0.0);
  REQUIRE(late < early * 0.05);
}

TEST_CASE("StereoDelay ping_pong step produces no single-sample spike", "[fx]") {
  // A block-rate (unsmoothed) ping_pong would inject a one-sample discontinuity
  // into the wet output at the step. With per-sample smoothing the largest
  // adjacent-sample change after the step stays on the order of the ambient
  // per-sample change already present in the steady-state signal.
  StereoDelay delay(StereoDelayConfig{kDelayMs, kDelayMs, kFeedback, 0.0f, kDryWet});
  delay.prepare(kSampleRate, 1);

  constexpr int kWarmup = 3000;
  constexpr int kMeasure = 400;

  const std::vector<float> before = run(delay, kWarmup, 200);
  REQUIRE(delay.set_parameter(3, 1.0f));
  const std::vector<float> after = run(delay, 0, kMeasure);

  auto max_adjacent_delta = [](const std::vector<float>& v) {
    float m = 0.0f;
    for (std::size_t i = 1; i < v.size(); ++i) {
      m = std::max(m, std::abs(v[i] - v[i - 1]));
    }
    return m;
  };

  const float baseline = max_adjacent_delta(before);
  const float stepped = max_adjacent_delta(after);
  // Smoothing keeps the step-induced jump comparable to the natural signal
  // slope (a hard step would be several multiples larger). This is an indirect
  // proxy: it bounds the discontinuity rather than measuring the private
  // smoothed value directly.
  REQUIRE(stepped < baseline * 3.0f + 1e-3f);
}
