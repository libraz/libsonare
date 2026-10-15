/// @file dattorro_reverb_test.cpp
/// @brief Tests for the Dattorro tank's damping as a physical quantity.

#include "effects/reverb/dattorro_reverb.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

using sonare::effects::reverb::DattorroReverb;
using sonare::effects::reverb::DattorroReverbConfig;

namespace {

std::vector<float> impulse_response(const DattorroReverbConfig& config, double sample_rate) {
  DattorroReverb reverb(config);
  reverb.prepare(sample_rate, 512);
  std::vector<float> left(static_cast<size_t>(0.5 * sample_rate), 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  left[0] = right[0] = 1.0f;
  float* channels[2] = {left.data(), right.data()};
  reverb.process(channels, 2, static_cast<int>(left.size()));
  return left;
}

/// Largest sample-wise difference relative to the response's peak.
double relative_difference(const std::vector<float>& a, const std::vector<float>& b) {
  double peak = 0.0;
  double diff = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    peak = std::max(peak, static_cast<double>(std::abs(a[i])));
    diff = std::max(diff, static_cast<double>(std::abs(a[i] - b[i])));
  }
  return diff / peak;
}

}  // namespace

TEST_CASE("Dattorro damping keeps its corner in Hz at every rate", "[dattorro][sample_rate]") {
  // damping 0.5 states d = 0.2 at the reference rate; that one-pole's half-power corner is the
  // physical quantity the tank holds.
  const float reference_coefficient = 0.5f * 0.4f;
  const double corner_hz = DattorroReverb::damping_corner_hz(
      reference_coefficient, DattorroReverb::kDampingReferenceSampleRate);
  REQUIRE(corner_hz > 1000.0);
  REQUIRE(corner_hz < 3000.0);
  // The corner and the coefficient are inverses at the reference rate.
  CHECK(DattorroReverb::damping_coefficient(
            corner_hz, DattorroReverb::kDampingReferenceSampleRate) == reference_coefficient);

  DattorroReverbConfig by_damping;
  by_damping.damping = 0.5f;
  by_damping.dry_wet = 1.0f;
  by_damping.mod_depth_samples = 0.0f;
  DattorroReverbConfig by_corner = by_damping;
  by_corner.damping_hz = static_cast<float>(corner_hz);

  for (double sr : {44100.0, 48000.0, 96000.0}) {
    INFO("sr " << sr);
    // The damping path renders what the corner path renders when asked for that corner, so its
    // corner is the requested one at each rate, and hence the same across rates.
    CHECK(relative_difference(impulse_response(by_damping, sr), impulse_response(by_corner, sr)) <=
          1e-4);
    const float coefficient = DattorroReverb::damping_coefficient(corner_hz, sr);
    CHECK(std::abs(DattorroReverb::damping_corner_hz(coefficient, sr) / corner_hz - 1.0) <= 1e-4);
  }

  // The coefficient itself does move with the rate: a raw per-sample value would not.
  CHECK(DattorroReverb::damping_coefficient(corner_hz, 44100.0) >
        DattorroReverb::damping_coefficient(corner_hz, 48000.0));
}

TEST_CASE("Dattorro damping automation rebuilds the coefficient at the working rate",
          "[dattorro][sample_rate]") {
  DattorroReverbConfig config;
  config.dry_wet = 1.0f;
  config.mod_depth_samples = 0.0f;
  config.damping = 0.9f;
  const std::vector<float> direct = impulse_response(config, 44100.0);

  config.damping = 0.1f;
  DattorroReverb reverb(config);
  reverb.prepare(44100.0, 512);
  REQUIRE(reverb.set_parameter(1, 0.9f));
  std::vector<float> left(direct.size(), 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  left[0] = right[0] = 1.0f;
  float* channels[2] = {left.data(), right.data()};
  reverb.process(channels, 2, static_cast<int>(left.size()));
  CHECK(left == direct);
}
