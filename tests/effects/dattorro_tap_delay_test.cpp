/// @file dattorro_tap_delay_test.cpp
/// @brief Output-tap timing contract for the Dattorro plate: a tap of N samples hears what the
///        tank wrote N samples earlier, not N - 1.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "effects/reverb/dattorro_reverb.h"

namespace {

using sonare::effects::reverb::DattorroReverb;
using sonare::effects::reverb::DattorroReverbConfig;

size_t scaled_tap(double ref_samples, double sample_rate) {
  return static_cast<size_t>(
      std::lround(ref_samples * sample_rate / DattorroReverb::kReferenceSampleRate));
}

int first_nonzero(const std::vector<float>& x) {
  for (size_t i = 0; i < x.size(); ++i) {
    if (x[i] != 0.0f) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

TEST_CASE("Dattorro output taps are exactly their table offset behind the tank write",
          "[effects][dattorro][tap-delay]") {
  for (const double sample_rate : {44100.0, 48000.0}) {
    INFO("sample_rate=" << sample_rate);
    // Decay 0 keeps the tank open-loop, so the first wet sample on each side is the nearest
    // direct tap on the first tank write: 266 (left, line 1) and 353 (right, line 1).
    DattorroReverbConfig config;
    config.decay = 0.0f;
    config.dry_wet = 1.0f;
    config.mod_depth_samples = 0.0f;
    config.pre_delay_samples = 0.0f;
    DattorroReverb reverb(config);
    reverb.prepare(sample_rate, 512);

    const int n = 2048;
    std::vector<float> left(n, 0.0f);
    std::vector<float> right(n, 0.0f);
    left[0] = 1.0f;
    right[0] = 1.0f;
    float* channels[2] = {left.data(), right.data()};
    reverb.process(channels, 2, n);

    CHECK(first_nonzero(left) == static_cast<int>(scaled_tap(266.0, sample_rate)));
    CHECK(first_nonzero(right) == static_cast<int>(scaled_tap(353.0, sample_rate)));
  }
}

TEST_CASE("Dattorro stays finite with the longest tank set", "[effects][dattorro][tap-delay]") {
  DattorroReverbConfig config;
  config.character = sonare::effects::reverb::kDattorroMaxCharacter;
  config.decay = 0.9f;
  DattorroReverb reverb(config);
  reverb.prepare(48000.0, 512);
  std::vector<float> left(48000, 0.0f);
  std::vector<float> right(48000, 0.0f);
  left[0] = right[0] = 1.0f;
  float* channels[2] = {left.data(), right.data()};
  reverb.process(channels, 2, 48000);
  for (const float v : left) REQUIRE(std::isfinite(v));
}
