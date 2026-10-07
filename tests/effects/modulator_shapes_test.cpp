/// @file modulator_shapes_test.cpp
/// @brief Modulator waveform choice on the auto-pan and ring modulator, the
///        ring modulator's channel phase, the phaser's sweep depth, and the
///        stereo balance's law and glide.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "effects/modulation/lfo.h"
#include "effects/modulation/phaser.h"
#include "effects/modulation/ring_modulator.h"
#include "mastering/stereo/auto_pan.h"
#include "mastering/stereo/stereo_balance.h"
#include "support/audio_fixtures.h"

namespace {

using sonare::effects::modulation::kLfoShapeCount;
using sonare::effects::modulation::lfo_shape_value;
using sonare::effects::modulation::LfoShape;
using sonare::effects::modulation::Phaser;
using sonare::effects::modulation::PhaserConfig;
using sonare::effects::modulation::RingModulator;
using sonare::effects::modulation::RingModulatorConfig;
using sonare::mastering::stereo::AutoPan;
using sonare::mastering::stereo::AutoPanConfig;
using sonare::mastering::stereo::StereoBalance;
using sonare::mastering::stereo::StereoBalanceConfig;
using sonare::mastering::stereo::StereoBalanceLaw;
using sonare::test::generate_sine;
using sonare::test::kRate;
using sonare::test::process_stereo;

constexpr int kSamples = 24000;

struct Stereo {
  std::vector<float> left;
  std::vector<float> right;
};

Stereo tone_pair() {
  const std::vector<float> a = generate_sine(kSamples, 330.0f, static_cast<int>(kRate), 0.4f);
  return {a, a};
}

Stereo run(sonare::rt::ProcessorBase& p, Stereo in) {
  p.prepare(kRate, kSamples);
  process_stereo(p, in.left, in.right);
  return in;
}

double max_diff(const std::vector<float>& a, const std::vector<float>& b) {
  double d = 0.0;
  for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
    d = std::max(d, static_cast<double>(std::fabs(a[i] - b[i])));
  }
  return d;
}

Stereo ones() { return {std::vector<float>(kSamples, 1.0f), std::vector<float>(kSamples, 1.0f)}; }

}  // namespace

TEST_CASE("lfo_shape_value follows each named waveform", "[modulator-shapes]") {
  REQUIRE(lfo_shape_value(LfoShape::kSine, 0.25) == Catch::Approx(1.0f));
  REQUIRE(lfo_shape_value(LfoShape::kTriangle, 0.0) == Catch::Approx(0.0f));
  REQUIRE(lfo_shape_value(LfoShape::kTriangle, 0.25) == Catch::Approx(1.0f));
  REQUIRE(lfo_shape_value(LfoShape::kTriangle, 0.5) == Catch::Approx(0.0f).margin(1e-6));
  REQUIRE(lfo_shape_value(LfoShape::kTriangle, 0.75) == Catch::Approx(-1.0f));
  REQUIRE(lfo_shape_value(LfoShape::kSquare, 0.1) == 1.0f);
  REQUIRE(lfo_shape_value(LfoShape::kSquare, 0.6) == -1.0f);
  REQUIRE(lfo_shape_value(LfoShape::kSawUp, 0.0) == Catch::Approx(-1.0f));
  REQUIRE(lfo_shape_value(LfoShape::kSawUp, 0.75) == Catch::Approx(0.5f));
  REQUIRE(lfo_shape_value(LfoShape::kSawDown, 0.75) == Catch::Approx(-0.5f));
  // The non-sine shapes wrap an unwrapped phase.
  REQUIRE(lfo_shape_value(LfoShape::kSawUp, 1.75) == Catch::Approx(0.5f));
}

TEST_CASE("auto pan: every shape sounds different, and the default is the sine",
          "[modulator-shapes]") {
  std::vector<Stereo> out;
  for (int s = 0; s < kLfoShapeCount; ++s) {
    AutoPanConfig config;
    config.rate_hz = 4.0f;
    config.shape = static_cast<LfoShape>(s);
    AutoPan pan(config);
    out.push_back(run(pan, tone_pair()));
  }
  for (std::size_t a = 0; a < out.size(); ++a) {
    for (std::size_t b = a + 1; b < out.size(); ++b) {
      INFO("shapes " << a << " and " << b);
      REQUIRE(max_diff(out[a].left, out[b].left) > 0.01);
    }
  }
  AutoPanConfig plain;
  plain.rate_hz = 4.0f;
  AutoPan pan(plain);
  REQUIRE(run(pan, tone_pair()).left == out[0].left);
}

TEST_CASE("auto pan: shape is realtime and refuses a fractional or unnamed value",
          "[modulator-shapes]") {
  AutoPan pan;
  REQUIRE(pan.parameter_is_realtime_safe(3));
  REQUIRE(pan.set_parameter(3, 4.0f));
  REQUIRE(pan.config().shape == LfoShape::kSawDown);
  REQUIRE_FALSE(pan.set_parameter(3, 1.5f));
  REQUIRE_FALSE(pan.set_parameter(3, 5.0f));
  REQUIRE_FALSE(pan.set_parameter(3, -1.0f));
  REQUIRE(pan.config().shape == LfoShape::kSawDown);
}

TEST_CASE("ring modulator: shape, channel phase and spread", "[modulator-shapes]") {
  RingModulatorConfig base;
  base.carrier_hz = 100.0f;
  std::vector<Stereo> out;
  for (int s = 0; s < kLfoShapeCount; ++s) {
    RingModulatorConfig config = base;
    config.shape = static_cast<LfoShape>(s);
    RingModulator rm(config);
    out.push_back(run(rm, tone_pair()));
  }
  for (std::size_t a = 0; a < out.size(); ++a) {
    for (std::size_t b = a + 1; b < out.size(); ++b) {
      INFO("shapes " << a << " and " << b);
      REQUIRE(max_diff(out[a].left, out[b].left) > 0.01);
    }
  }
  // A square carrier is +-1, so a constant input comes out as +-1.
  {
    RingModulatorConfig config = base;
    config.shape = LfoShape::kSquare;
    RingModulator rm(config);
    const Stereo r = run(rm, ones());
    for (const float v : r.left) REQUIRE(std::fabs(std::fabs(v) - 1.0f) < 1e-6f);
  }
  // Shared carrier by default.
  {
    RingModulator rm(base);
    const Stereo r = run(rm, tone_pair());
    REQUIRE(r.left == r.right);
  }
  // 180 degrees of phase, and a full spread, both turn the right carrier over.
  RingModulatorConfig phased = base;
  phased.phase_deg = 180.0f;
  RingModulator rm_phase(phased);
  const Stereo p = run(rm_phase, ones());
  for (std::size_t i = 0; i < p.left.size(); ++i) {
    REQUIRE(p.right[i] == Catch::Approx(-p.left[i]).margin(1e-4));
  }
  RingModulatorConfig spread = base;
  spread.stereo_spread = 1.0f;
  RingModulator rm_spread(spread);
  const Stereo q = run(rm_spread, ones());
  REQUIRE(max_diff(q.right, p.right) < 1e-4);
  // A partial spread lands between: audibly not the shared carrier.
  spread.stereo_spread = 0.25f;
  RingModulator rm_part(spread);
  const Stereo h = run(rm_part, ones());
  REQUIRE(max_diff(h.left, h.right) > 0.1);
}

TEST_CASE("ring modulator: new keys are realtime and validated", "[modulator-shapes]") {
  RingModulator rm;
  for (unsigned id = 2; id <= 4; ++id) REQUIRE(rm.parameter_is_realtime_safe(id));
  REQUIRE(rm.set_parameter(2, 2.0f));
  REQUIRE(rm.set_parameter(3, 90.0f));
  REQUIRE(rm.set_parameter(4, 0.5f));
  REQUIRE_FALSE(rm.set_parameter(2, 0.5f));
  REQUIRE_FALSE(rm.set_parameter(2, 5.0f));
  REQUIRE(rm.set_parameter(5, 1.0f));  // mix law
  REQUIRE_FALSE(rm.set_parameter(5, 0.5f));
  REQUIRE_FALSE(rm.set_parameter(6, 0.0f));
}

TEST_CASE("phaser: depth sweeps from minHz to minHz * (maxHz / minHz)^depth",
          "[modulator-shapes]") {
  PhaserConfig base;
  base.dry_wet = 1.0f;
  base.rate_hz = 2.0f;
  base.min_hz = 300.0f;
  base.max_hz = 1600.0f;

  Phaser full(base);
  const Stereo reference = run(full, tone_pair());

  // Depth 1 spelled out is the default, bit for bit.
  PhaserConfig one = base;
  one.depth = 1.0f;
  Phaser full_explicit(one);
  REQUIRE(run(full_explicit, tone_pair()).left == reference.left);

  // A partial depth is the same sweep as a phaser whose maxHz sits at the endpoint.
  for (const float depth : {0.25f, 0.5f, 0.8f}) {
    PhaserConfig deep = base;
    deep.depth = depth;
    Phaser a(deep);
    PhaserConfig ended = base;
    ended.max_hz = base.min_hz * std::pow(base.max_hz / base.min_hz, depth);
    Phaser b(ended);
    const Stereo ra = run(a, tone_pair());
    const Stereo rb = run(b, tone_pair());
    INFO("depth " << depth);
    REQUIRE(max_diff(ra.left, rb.left) < 1e-5);
    REQUIRE(max_diff(ra.right, rb.right) < 1e-5);
    REQUIRE(max_diff(ra.left, reference.left) > 0.01);
  }

  // Depth 0 holds the notches at minHz: the sweep rate no longer matters.
  PhaserConfig flat_slow = base;
  flat_slow.depth = 0.0f;
  flat_slow.rate_hz = 0.5f;
  PhaserConfig flat_fast = flat_slow;
  flat_fast.rate_hz = 5.0f;
  Phaser slow(flat_slow);
  Phaser fast(flat_fast);
  REQUIRE(max_diff(run(slow, tone_pair()).left, run(fast, tone_pair()).left) < 1e-6);
}

TEST_CASE("phaser: depth is realtime and clamped", "[modulator-shapes]") {
  Phaser p;
  REQUIRE(p.parameter_is_realtime_safe(5));
  REQUIRE(p.set_parameter(5, 2.0f));
  REQUIRE(p.set_parameter(5, -1.0f));
  REQUIRE_FALSE(p.set_parameter(6, 0.0f));
}

TEST_CASE("stereo balance: the raw law puts the centre at -3 dB", "[modulator-shapes]") {
  StereoBalanceConfig normalized;
  StereoBalance a(normalized);
  const Stereo n = run(a, ones());
  REQUIRE(n.left.back() == Catch::Approx(1.0f));

  StereoBalanceConfig raw;
  raw.law = StereoBalanceLaw::kRawConstantPower;
  StereoBalance b(raw);
  const Stereo r = run(b, ones());
  REQUIRE(r.left.back() == Catch::Approx(std::sqrt(0.5f)));
  REQUIRE(r.right.back() == Catch::Approx(std::sqrt(0.5f)));

  raw.balance = 1.0f;
  StereoBalance c(raw);
  const Stereo hard = run(c, ones());
  REQUIRE(hard.left.back() == Catch::Approx(0.0f).margin(1e-6));
  REQUIRE(hard.right.back() == Catch::Approx(1.0f));

  StereoBalance d;
  REQUIRE(d.parameter_is_realtime_safe(1));
  REQUIRE(d.set_parameter(1, 1.0f));
  REQUIRE_FALSE(d.set_parameter(1, 0.5f));
  REQUIRE_FALSE(d.set_parameter(1, 2.0f));
}

TEST_CASE("stereo balance: a change glides over 5 ms and never steps", "[modulator-shapes]") {
  StereoBalance balance;
  balance.prepare(kRate, 512);
  Stereo io = ones();
  process_stereo(balance, io.left, io.right);
  REQUIRE(io.left[0] == Catch::Approx(1.0f));
  balance.set_parameter(0, 1.0f);
  io = ones();
  process_stereo(balance, io.left, io.right);
  const int ramp = static_cast<int>(std::lround(0.005 * kRate));
  // Monotone descent to 0 with no step larger than the linear increment, done by 5 ms.
  float largest_step = 0.0f;
  for (int i = 1; i < kSamples; ++i) {
    REQUIRE(io.left[static_cast<std::size_t>(i)] <=
            io.left[static_cast<std::size_t>(i) - 1] + 1e-7f);
    largest_step = std::max(largest_step, io.left[static_cast<std::size_t>(i) - 1] -
                                              io.left[static_cast<std::size_t>(i)]);
  }
  REQUIRE(largest_step <= 1.5f / static_cast<float>(ramp));
  REQUIRE(io.left[static_cast<std::size_t>(ramp) - 1] == Catch::Approx(0.0f).margin(1e-6));
  REQUIRE(io.left[static_cast<std::size_t>(ramp) / 2] > 0.1f);
}
