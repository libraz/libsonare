/// @file pan_law_test.cpp
/// @brief Contract of the shared pan-law evaluator and of the paths that use it.

#include "mixing/pan_law.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mixing/panner.h"

using Catch::Matchers::WithinAbs;
using sonare::mixing::compute_pan_gains;
using sonare::mixing::kPanLawCount;
using sonare::mixing::pan_law_from_index;
using sonare::mixing::PanGains;
using sonare::mixing::PanLaw;
using sonare::mixing::PannerConfig;
using sonare::mixing::PannerProcessor;
using sonare::mixing::PanNormalization;

namespace {

constexpr std::array<PanLaw, 4> kAllLaws = {PanLaw::Const3dB, PanLaw::Const4p5dB, PanLaw::Const6dB,
                                            PanLaw::Linear0dB};

// Pan positions swept by the range tests, hard left through hard right.
std::vector<float> pan_sweep() {
  std::vector<float> pans;
  for (int i = -20; i <= 20; ++i) {
    pans.push_back(static_cast<float>(i) / 20.0f);
  }
  return pans;
}

// Steady-state gain pair the PannerProcessor applies in Balance mode. prepare()
// snaps the smoothers to the configured pan, so a DC input reads the settled
// gains straight off the first sample.
PanGains balance_mode_gains(float pan, PanLaw law) {
  PannerProcessor panner(PannerConfig{pan, law, 5.0f});
  panner.prepare(48000.0, 8);
  std::vector<float> left(8, 1.0f);
  std::vector<float> right(8, 1.0f);
  float* planes[2] = {left.data(), right.data()};
  panner.process(planes, 2, 8);
  return {left[0], right[0]};
}

}  // namespace

TEST_CASE("constant-power pan law conserves energy across the pan range", "[mixing][pan]") {
  for (const float pan : pan_sweep()) {
    const PanGains g = compute_pan_gains(pan, PanLaw::Const3dB, PanNormalization::Raw);
    REQUIRE_THAT(g.left * g.left + g.right * g.right, WithinAbs(1.0f, 1e-5f));
  }
}

TEST_CASE("centre-unity normalization is unity at centre for every law", "[mixing][pan]") {
  for (const PanLaw law : kAllLaws) {
    const PanGains g = compute_pan_gains(0.0f, law, PanNormalization::CenterUnity);
    REQUIRE_THAT(g.left, WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(g.right, WithinAbs(1.0f, 1e-5f));
  }
}

TEST_CASE("centre-unity constant-power law holds total stereo energy", "[mixing][pan]") {
  // Both channels pass a centred signal at unity, so the pair carries twice the
  // per-channel input energy; that total must not move as the pan sweeps. This
  // is what keeps a pan modulator from breathing at its own LFO rate.
  for (const float pan : pan_sweep()) {
    const PanGains g = compute_pan_gains(pan, PanLaw::Const3dB, PanNormalization::CenterUnity);
    REQUIRE_THAT(g.left * g.left + g.right * g.right, WithinAbs(2.0f, 1e-5f));
  }
}

TEST_CASE("near-unity normalization pins the near channel at unity", "[mixing][pan]") {
  for (const PanLaw law : kAllLaws) {
    for (const float pan : pan_sweep()) {
      const PanGains g = compute_pan_gains(pan, law, PanNormalization::NearUnity);
      const float near_gain = pan <= 0.0f ? g.left : g.right;
      const float away_gain = pan <= 0.0f ? g.right : g.left;
      REQUIRE_THAT(near_gain, WithinAbs(1.0f, 1e-5f));
      REQUIRE(away_gain <= near_gain + 1e-5f);
    }
  }
}

TEST_CASE("linear balance is the near-unity form of the 0 dB law", "[mixing][pan]") {
  // The clip player and the lane mixer historically hardcoded this formula. It
  // has to stay reachable through the shared evaluator so those paths can drop
  // their private copies without moving a single sample.
  for (const float pan : pan_sweep()) {
    const PanGains g = compute_pan_gains(pan, PanLaw::Linear0dB, PanNormalization::NearUnity);
    REQUIRE_THAT(g.left, WithinAbs(pan > 0.0f ? 1.0f - pan : 1.0f, 1e-6f));
    REQUIRE_THAT(g.right, WithinAbs(pan < 0.0f ? 1.0f + pan : 1.0f, 1e-6f));
  }
}

TEST_CASE("panner balance mode applies the shared evaluator's near-unity gains", "[mixing][pan]") {
  for (const PanLaw law : kAllLaws) {
    for (const float pan : pan_sweep()) {
      const PanGains expected = compute_pan_gains(pan, law, PanNormalization::NearUnity);
      const PanGains actual = balance_mode_gains(pan, law);
      REQUIRE_THAT(actual.left, WithinAbs(expected.left, 1e-5f));
      REQUIRE_THAT(actual.right, WithinAbs(expected.right, 1e-5f));
    }
  }
}

TEST_CASE("the wire encoding of a pan law is the enum's declaration order", "[mixing][pan]") {
  // Every caller that carries a law as an integer — the mixer scene, the C ABI,
  // the bindings, the engine strip specs — decodes it here, so this mapping is
  // the wire format and not an implementation detail.
  REQUIRE(kPanLawCount == static_cast<int>(kAllLaws.size()));
  for (int index = 0; index < kPanLawCount; ++index) {
    REQUIRE(pan_law_from_index(index) == kAllLaws[static_cast<size_t>(index)]);
  }

  // The fallback the decoder documents: an encoding outside the named range
  // resolves to the constant-power default rather than to an unnamed law.
  REQUIRE(pan_law_from_index(-1) == PanLaw::Const3dB);
  REQUIRE(pan_law_from_index(kPanLawCount) == PanLaw::Const3dB);
  REQUIRE(pan_law_from_index(4) == PanLaw::Const3dB);
}
