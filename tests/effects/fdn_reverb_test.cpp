/// @file fdn_reverb_test.cpp
/// @brief Tests for the feedback delay network reverb.

#include "effects/reverb/fdn_reverb.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using sonare::effects::reverb::FdnReverb;
using sonare::effects::reverb::FdnReverbConfig;

namespace {

/// @brief Builds a short impulse buffer (unit sample at index 0).
std::vector<float> make_impulse(int n) {
  std::vector<float> buf(static_cast<size_t>(n), 0.0f);
  if (n > 0) buf[0] = 1.0f;
  return buf;
}

}  // namespace

// Regression for the mono output clobber bug: when num_channels == 1 the right
// channel aliases the left buffer, so writing both taps to the same address used
// to drop the first tap. The mono result must be the fold-down of BOTH taps, i.e.
// 0.5 * (out_l + out_r), which equals 0.5 * (stereo_left + stereo_right) when the
// stereo run is fed the same input on both channels.
TEST_CASE("FdnReverb mono output folds down both taps", "[reverb][fdn]") {
  constexpr int kN = 4096;
  FdnReverbConfig config;
  config.dry_wet = 1.0f;  // Fully wet so the tap mix is observable.

  // Mono run.
  FdnReverb mono(config);
  mono.prepare(48000.0, kN);
  std::vector<float> mono_buf = make_impulse(kN);
  float* mono_ch[1] = {mono_buf.data()};
  mono.process(mono_ch, 1, kN);

  // Stereo run with identical input on both channels. The internal network is
  // driven by 0.5 * (in_l + in_r) == in, so its state matches the mono run.
  FdnReverb stereo(config);
  stereo.prepare(48000.0, kN);
  std::vector<float> left = make_impulse(kN);
  std::vector<float> right = make_impulse(kN);
  float* stereo_ch[2] = {left.data(), right.data()};
  stereo.process(stereo_ch, 2, kN);

  // The mono output must equal the fold-down 0.5 * (L + R) of the stereo taps,
  // not just the second tap (stereo right channel) on its own.
  double mono_energy = 0.0;
  double diff_from_fold = 0.0;
  double diff_from_second_tap = 0.0;
  for (int i = 0; i < kN; ++i) {
    const float fold = 0.5f * (left[static_cast<size_t>(i)] + right[static_cast<size_t>(i)]);
    const float m = mono_buf[static_cast<size_t>(i)];
    mono_energy += static_cast<double>(m) * m;
    diff_from_fold += std::abs(static_cast<double>(m) - fold);
    diff_from_second_tap += std::abs(static_cast<double>(m) - right[static_cast<size_t>(i)]);
  }

  // Reverb tail carries energy from the impulse.
  REQUIRE(mono_energy > 1e-6);
  // Mono equals the proper two-tap fold-down (exact: same network state).
  REQUIRE_THAT(diff_from_fold, WithinAbs(0.0, 1e-4));
  // And it is genuinely different from the buggy "second tap only" behaviour,
  // confirming the first tap is not being clobbered.
  REQUIRE(diff_from_second_tap > 1e-3);
}
