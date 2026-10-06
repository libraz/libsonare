/// @file pan_law_test.cpp
/// @brief Contract of the shared pan-law evaluator and of the paths that use it.

#include "mixing/pan_law.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <utility>
#include <vector>

#include "mixing/panner.h"
#include "rt/param_smoother.h"

using Catch::Matchers::WithinAbs;
using sonare::mixing::compute_pan_gains;
using sonare::mixing::kPanLawCount;
using sonare::mixing::pan_law_from_index;
using sonare::mixing::PanGains;
using sonare::mixing::PanLaw;
using sonare::mixing::PanMode;
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

TEST_CASE("panner tolerates an unbound plane in every mode", "[mixing][pan]") {
  // The mixing layer hands over partially-bound plane tables (the engine's
  // monitor bus, a strip fed a single wired channel), and every sibling
  // processor tolerates that. The mono short-circuit used to dereference
  // channels[0] before reaching the stereo branch's null check, so a mono
  // configuration with an unbound plane crashed inside the audio callback.
  constexpr int kSamples = 8;
  const std::array<PanMode, 3> kModes{PanMode::Balance, PanMode::StereoPan, PanMode::DualPan};

  for (const PanMode mode : kModes) {
    for (const float pan : {-0.75f, 0.0f, 0.75f}) {
      CAPTURE(static_cast<int>(mode));
      CAPTURE(pan);

      std::vector<float> plane(kSamples, 0.5f);

      // Mono with an unbound plane: the branch that used to crash.
      {
        PannerProcessor panner;
        panner.prepare(48000.0, kSamples);
        panner.set_pan_mode(mode);
        panner.set_pan(pan);
        float* channels[] = {nullptr};
        panner.process(channels, 1, kSamples);
      }

      // Stereo with either plane unbound, and with the whole table unbound.
      {
        PannerProcessor panner;
        panner.prepare(48000.0, kSamples);
        panner.set_pan_mode(mode);
        panner.set_pan(pan);
        float* left_unbound[] = {nullptr, plane.data()};
        float* right_unbound[] = {plane.data(), nullptr};
        float* both_unbound[] = {nullptr, nullptr};
        panner.process(left_unbound, 2, kSamples);
        panner.process(right_unbound, 2, kSamples);
        panner.process(both_unbound, 2, kSamples);
      }

      // The bound plane of a partially-bound stereo pair is left untouched, so
      // the guard skips rather than writing through a half-valid table.
      for (float sample : plane) {
        REQUIRE_THAT(sample, WithinAbs(0.5f, 1e-6f));
      }
    }
  }

  // A fully bound mono buffer still gets its gain, so the guard did not turn
  // the mono path into a no-op.
  std::vector<float> mono(kSamples, 1.0f);
  float* channels[] = {mono.data()};
  PannerProcessor panner;
  panner.prepare(48000.0, kSamples);
  panner.set_pan(0.0f);
  panner.process(channels, 1, kSamples);
  for (float sample : mono) {
    REQUIRE(std::isfinite(sample));
    REQUIRE(sample > 0.0f);
  }
}

TEST_CASE("panner at-rest identity reflects mode, pan target, and settle state", "[mixing][pan]") {
  // Fresh construction settles at pan 0 in Balance mode, for every law.
  for (const PanLaw law : kAllLaws) {
    PannerProcessor panner(PannerConfig{0.0f, law, 5.0f});
    panner.prepare(48000.0, 64);
    REQUIRE(panner.at_rest_identity());
  }

  // A non-Balance mode is never the identity, even centred.
  PannerProcessor stereo_pan(PannerConfig{0.0f, PanLaw::Const3dB, 5.0f, PanMode::StereoPan});
  stereo_pan.prepare(48000.0, 64);
  REQUIRE_FALSE(stereo_pan.at_rest_identity());

  PannerProcessor dual_pan(PannerConfig{0.0f, PanLaw::Const3dB, 5.0f, PanMode::DualPan});
  dual_pan.prepare(48000.0, 64);
  REQUIRE_FALSE(dual_pan.at_rest_identity());

  // A non-zero pan target is never the identity.
  PannerProcessor panned(PannerConfig{0.3f, PanLaw::Const3dB, 5.0f});
  panned.prepare(48000.0, 64);
  REQUIRE_FALSE(panned.at_rest_identity());

  // Sending the target back to 0 without letting the smoother catch up leaves
  // the panner still gliding from the earlier pan, so a caller reading the
  // target alone would wrongly call this the identity and freeze mid-glide.
  PannerProcessor gliding(PannerConfig{0.0f, PanLaw::Const3dB, 5.0f});
  gliding.prepare(48000.0, 64);
  gliding.set_pan(0.3f);
  std::array<float, 1> one_left{1.0f};
  std::array<float, 1> one_right{1.0f};
  float* one_sample[] = {one_left.data(), one_right.data()};
  gliding.process(one_sample, 2, 1);
  gliding.set_pan(0.0f);
  REQUIRE_FALSE(gliding.at_rest_identity());

  // Settling the smoothers (as ChannelStrip::settle() does) restores it.
  gliding.reset();
  REQUIRE(gliding.at_rest_identity());

  // A pan settled in place (as an offline render does), then centred, must still
  // glide back: the smoothers' targets only move inside process().
  PannerProcessor settled(PannerConfig{0.0f, PanLaw::Const3dB, 5.0f});
  settled.prepare(48000.0, 64);
  settled.set_pan(0.3f);
  settled.reset();
  settled.set_pan(0.0f);
  REQUIRE_FALSE(settled.at_rest_identity());
}

namespace {

// Panner paths exercised by the glide tests. Mono is the single-plane path of the
// same processor, so it is listed beside the three stereo modes.
enum class GlidePath { Balance, StereoPan, DualPan, Mono };

constexpr std::array<GlidePath, 4> kGlidePaths = {GlidePath::Balance, GlidePath::StereoPan,
                                                  GlidePath::DualPan, GlidePath::Mono};

constexpr double kGlideRate = 48000.0;
constexpr float kGlideSmoothingMs = 5.0f;
constexpr float kGlideInputLeft = 1.0f;
constexpr float kGlideInputRight = 0.5f;

PanMode glide_mode(GlidePath path) {
  switch (path) {
    case GlidePath::StereoPan:
      return PanMode::StereoPan;
    case GlidePath::DualPan:
      return PanMode::DualPan;
    default:
      return PanMode::Balance;
  }
}

struct GlideOutput {
  std::vector<float> left;
  std::vector<float> right;
};

// Hard-left to hard-right jump (dual pan: the two positions swap) rendered in
// blocks of @p block samples. The panner is settled at the start position by
// prepare(), then the new position is set once.
GlideOutput render_glide(GlidePath path, PanLaw law, int num_samples, int block) {
  PannerProcessor panner(PannerConfig{-1.0f, law, kGlideSmoothingMs, glide_mode(path)});
  panner.prepare(kGlideRate, block);
  if (path == GlidePath::DualPan) {
    panner.set_dual_pan(1.0f, -1.0f);
  } else {
    panner.set_pan(1.0f);
  }
  const bool mono = path == GlidePath::Mono;
  GlideOutput out{std::vector<float>(static_cast<size_t>(num_samples), kGlideInputLeft),
                  std::vector<float>(static_cast<size_t>(num_samples), kGlideInputRight)};
  for (int offset = 0; offset < num_samples; offset += block) {
    float* planes[2] = {out.left.data() + offset, out.right.data() + offset};
    panner.process(planes, mono ? 1 : 2, std::min(block, num_samples - offset));
  }
  return out;
}

// Output a static placement at the given positions produces, from the law's own
// evaluator. @p pan drives Balance / StereoPan / Mono; the dual positions drive
// DualPan.
std::pair<float, float> static_output(GlidePath path, PanLaw law, float pan, float dual_left,
                                      float dual_right) {
  const float in_l = kGlideInputLeft;
  const float in_r = kGlideInputRight;
  switch (path) {
    case GlidePath::Balance: {
      const PanGains g = compute_pan_gains(pan, law, PanNormalization::NearUnity);
      return {in_l * g.left, in_r * g.right};
    }
    case GlidePath::StereoPan: {
      const PanGains g = compute_pan_gains(pan, law);
      const float mono = 0.5f * (in_l + in_r);
      return {mono * g.left, mono * g.right};
    }
    case GlidePath::DualPan: {
      const PanGains a = compute_pan_gains(dual_left, law);
      const PanGains b = compute_pan_gains(dual_right, law);
      return {in_l * a.left + in_r * b.left, in_l * a.right + in_r * b.right};
    }
    case GlidePath::Mono: {
      const PanGains g = compute_pan_gains(pan, law);
      return {in_l * std::sqrt(g.left * g.left + g.right * g.right), in_r};
    }
  }
  return {0.0f, 0.0f};
}

}  // namespace

TEST_CASE("panner glide stays on the active pan law at every sample", "[mixing][pan]") {
  constexpr int kSamples = 2400;
  for (const GlidePath path : kGlidePaths) {
    for (const PanLaw law : kAllLaws) {
      CAPTURE(static_cast<int>(path));
      CAPTURE(static_cast<int>(law));
      const GlideOutput out = render_glide(path, law, kSamples, kSamples);

      // The position the panner is entitled to be at: the same one-pole the
      // engine lane pan uses, driven from the old position to the new one.
      sonare::rt::ParamSmoother pan_ref(-1.0f, kGlideSmoothingMs, kGlideRate);
      sonare::rt::ParamSmoother dual_left_ref(-1.0f, kGlideSmoothingMs, kGlideRate);
      sonare::rt::ParamSmoother dual_right_ref(1.0f, kGlideSmoothingMs, kGlideRate);
      pan_ref.set_target(1.0f);
      dual_left_ref.set_target(1.0f);
      dual_right_ref.set_target(-1.0f);

      float worst = 0.0f;
      for (int i = 0; i < kSamples; ++i) {
        const auto expected = static_output(path, law, pan_ref.process(), dual_left_ref.process(),
                                            dual_right_ref.process());
        const size_t k = static_cast<size_t>(i);
        worst = std::max(worst, std::abs(out.left[k] - expected.first));
        if (path != GlidePath::Mono) {
          worst = std::max(worst, std::abs(out.right[k] - expected.second));
        }
      }
      REQUIRE(worst < 1e-5f);

      // The jump really travelled (a constant-power mono gain is flat by design).
      if (path != GlidePath::Mono || law != PanLaw::Const3dB) {
        REQUIRE(out.left.front() != out.left.back());
      }
    }
  }
}

TEST_CASE("panner constant-power glide holds the law's loudness invariant", "[mixing][pan]") {
  constexpr int kSamples = 2400;

  // -3 dB law: L^2 + R^2 stays at the static constant through a full jump, for
  // both the stereo-pan and the mono path (the mono gain is sqrt(L^2 + R^2)).
  const GlideOutput stereo = render_glide(GlidePath::StereoPan, PanLaw::Const3dB, kSamples, 64);
  const GlideOutput mono = render_glide(GlidePath::Mono, PanLaw::Const3dB, kSamples, 64);
  const float mono_in = 0.5f * (kGlideInputLeft + kGlideInputRight);
  for (int i = 0; i < kSamples; ++i) {
    const size_t k = static_cast<size_t>(i);
    const float l = stereo.left[k] / mono_in;
    const float r = stereo.right[k] / mono_in;
    REQUIRE_THAT(l * l + r * r, WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(mono.left[k] / kGlideInputLeft, WithinAbs(1.0f, 1e-5f));
  }

  // -6 dB law: L + R stays at the static constant.
  const GlideOutput linear = render_glide(GlidePath::StereoPan, PanLaw::Const6dB, kSamples, 64);
  for (int i = 0; i < kSamples; ++i) {
    const size_t k = static_cast<size_t>(i);
    REQUIRE_THAT((linear.left[k] + linear.right[k]) / mono_in, WithinAbs(1.0f, 1e-5f));
  }
}

TEST_CASE("panner glide output does not depend on block partitioning", "[mixing][pan]") {
  constexpr int kSamples = 64;
  for (const GlidePath path : kGlidePaths) {
    for (const PanLaw law : kAllLaws) {
      CAPTURE(static_cast<int>(path));
      CAPTURE(static_cast<int>(law));
      const GlideOutput whole = render_glide(path, law, kSamples, 64);
      const GlideOutput split = render_glide(path, law, kSamples, 16);
      REQUIRE(whole.left == split.left);
      REQUIRE(whole.right == split.right);
    }
  }
}

TEST_CASE("panner settles on the static law output after a glide", "[mixing][pan]") {
  // Long enough for the smoother to run out of float resolution, which is where
  // an unfinished one-pole would leave a residual against the static placement.
  constexpr int kSamples = 96000;
  for (const GlidePath path : kGlidePaths) {
    for (const PanLaw law : kAllLaws) {
      CAPTURE(static_cast<int>(path));
      CAPTURE(static_cast<int>(law));
      const GlideOutput out = render_glide(path, law, kSamples, 480);
      const auto expected = static_output(path, law, 1.0f, 1.0f, -1.0f);
      REQUIRE_THAT(out.left.back(), WithinAbs(expected.first, 1e-7f));
      if (path != GlidePath::Mono) {
        REQUIRE_THAT(out.right.back(), WithinAbs(expected.second, 1e-7f));
      }
    }
  }
}

TEST_CASE("panner is the at-rest identity again once a glide back to centre has run",
          "[mixing][pan]") {
  PannerProcessor panner(PannerConfig{0.7f, PanLaw::Const3dB, kGlideSmoothingMs});
  panner.prepare(kGlideRate, 480);
  panner.set_pan(0.0f);
  std::vector<float> left(96000, 1.0f);
  std::vector<float> right(96000, 1.0f);
  float* planes[2] = {left.data(), right.data()};
  panner.process(planes, 2, 96000);
  REQUIRE(panner.at_rest_identity());
}
