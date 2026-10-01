/// @file reverb_character_test.cpp
/// @brief The plate reverb's character sets, corner-in-Hz damping and output gate.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

#include "effects/reverb/dattorro_reverb.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::effects::reverb::DattorroGateType;
using sonare::effects::reverb::DattorroReverb;
using sonare::effects::reverb::DattorroReverbConfig;
using sonare::effects::reverb::kDattorroMaxCharacter;

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

DattorroReverbConfig wet_config() {
  DattorroReverbConfig config;
  config.dry_wet = 1.0f;
  config.mod_depth_samples = 0.0f;  // a time-invariant tank, so a render is a fixed response
  return config;
}

struct Render {
  std::vector<float> left;
  std::vector<float> right;
};

/// Impulse response of the given configuration, rendered block by block.
Render impulse_response(const DattorroReverbConfig& config, double seconds, double rate = kRate) {
  DattorroReverb reverb(config);
  reverb.prepare(rate, kBlock);
  const int total = static_cast<int>(seconds * rate) / kBlock * kBlock;
  Render out{std::vector<float>(static_cast<std::size_t>(total), 0.0f),
             std::vector<float>(static_cast<std::size_t>(total), 0.0f)};
  out.left[0] = out.right[0] = 1.0f;
  for (int offset = 0; offset < total; offset += kBlock) {
    float* channels[2] = {out.left.data() + offset, out.right.data() + offset};
    reverb.process(channels, 2, kBlock);
  }
  return out;
}

double energy(const std::vector<float>& x, double from_s, double to_s, double rate = kRate) {
  const std::size_t a = static_cast<std::size_t>(from_s * rate);
  const std::size_t b = std::min(x.size(), static_cast<std::size_t>(to_s * rate));
  double sum = 0.0;
  for (std::size_t i = a; i < b; ++i) sum += static_cast<double>(x[i]) * x[i];
  return sum;
}

double peak(const std::vector<float>& x, double from_s, double to_s) {
  const std::size_t a = static_cast<std::size_t>(from_s * kRate);
  const std::size_t b = std::min(x.size(), static_cast<std::size_t>(to_s * kRate));
  float p = 0.0f;
  for (std::size_t i = a; i < b; ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}

/// Fraction of the response's energy that lies in the first @p early_s seconds; a
/// denser, shorter tank spends its energy sooner.
double early_fraction(const std::vector<float>& x, double early_s) {
  return energy(x, 0.0, early_s) / energy(x, 0.0, 10.0);
}

}  // namespace

TEST_CASE("each character gives its own stable tail", "[reverb-character]") {
  DattorroReverbConfig config = wet_config();
  config.decay = 0.7f;

  std::vector<Render> renders;
  for (int c = 0; c <= kDattorroMaxCharacter; ++c) {
    config.character = c;
    renders.push_back(impulse_response(config, 1.5));
    const Render again = impulse_response(config, 1.5);
    INFO("character " << c);
    REQUIRE(renders.back().left == again.left);  // stable
    REQUIRE(renders.back().right == again.right);
    REQUIRE(sonare::test::peak_abs(renders.back().left) > 0.0f);
  }
  for (int a = 0; a <= kDattorroMaxCharacter; ++a) {
    for (int b = a + 1; b <= kDattorroMaxCharacter; ++b) {
      INFO("characters " << a << " and " << b);
      REQUIRE(sonare::test::max_abs_difference(renders[static_cast<std::size_t>(a)].left,
                                               renders[static_cast<std::size_t>(b)].left) > 1e-3f);
    }
  }

  // Longer delay lines, sparser and longer tails: the reported tail grows through
  // Room 1 ... Hall 2, and the energy spent in the first 0.3 s falls with it.
  int previous_tail = 0;
  double previous_early = 2.0;
  for (int c = 1; c <= kDattorroMaxCharacter; ++c) {
    config.character = c;
    DattorroReverb reverb(config);
    reverb.prepare(kRate, kBlock);
    INFO("character " << c);
    REQUIRE(reverb.tail_samples() > previous_tail);
    previous_tail = reverb.tail_samples();
    const double early = early_fraction(renders[static_cast<std::size_t>(c)].left, 0.3);
    REQUIRE(early < previous_early);
    previous_early = early;
  }

  // An out-of-range value falls back to the nearest set rather than reaching a table off its end.
  config.character = 99;
  const Render clamped = impulse_response(config, 0.5);
  config.character = kDattorroMaxCharacter;
  const Render top = impulse_response(config, 0.5);
  REQUIRE(clamped.left == top.left);
}

TEST_CASE("the defaults leave the tank as it was", "[reverb-character]") {
  DattorroReverbConfig defaults = wet_config();
  DattorroReverbConfig spelled = wet_config();
  spelled.damping_hz = 0.0f;
  spelled.gate_threshold_db = -120.0f;
  spelled.gate_hold_ms = 100.0f;
  spelled.gate_type = DattorroGateType::kNormal;
  spelled.character = 0;
  REQUIRE(impulse_response(defaults, 0.5).left == impulse_response(spelled, 0.5).left);

  // The gate is inert under the off threshold whatever its other fields say.
  spelled.gate_hold_ms = 5.0f;
  spelled.gate_type = DattorroGateType::kSweep2;
  REQUIRE(impulse_response(defaults, 0.5).left == impulse_response(spelled, 0.5).left);

  DattorroReverb canonical(DattorroReverbConfig{});
  canonical.prepare(kRate, kBlock);
  REQUIRE(canonical.tail_samples() == 86752);
}

TEST_CASE("the damping corner is a half-power point at every rate", "[reverb-character]") {
  for (const double rate : {44100.0, 48000.0, 96000.0}) {
    for (const double corner : {200.0, 1000.0, 5000.0, 12000.0}) {
      const double a = DattorroReverb::damping_coefficient(corner, rate);
      const double w = kTwoPiD * corner / rate;
      // |H|^2 of y += a (x - y) at w.
      const double b = 1.0 - a;
      const double h2 = a * a / (1.0 - 2.0 * b * std::cos(w) + b * b);
      INFO("corner " << corner << " Hz at " << rate);
      REQUIRE(h2 == Catch::Approx(0.5).epsilon(1e-4));
    }
  }
}

TEST_CASE("a lower damping corner darkens the tail", "[reverb-character]") {
  const auto tail_centroid = [](double corner) {
    const double rate = kRate;
    DattorroReverbConfig config = wet_config();
    config.decay = 0.8f;
    config.damping_hz = static_cast<float>(corner);
    const Render r = impulse_response(config, 1.0, rate);
    // Centroid over a late window, by direct DFT on a coarse grid.
    const std::size_t from = static_cast<std::size_t>(0.4 * rate);
    const std::size_t to = static_cast<std::size_t>(0.9 * rate);
    double num = 0.0;
    double den = 0.0;
    for (double f = 100.0; f < 16000.0; f *= 1.06) {
      std::complex<double> sum(0.0, 0.0);
      for (std::size_t n = from; n < to; ++n) {
        sum += static_cast<double>(r.left[n]) * std::polar(1.0, -kTwoPiD * f * n / rate);
      }
      const double power = std::norm(sum) * f;  // log-spaced grid: weight by bin width
      num += power * f;
      den += power;
    }
    return num / den;
  };
  const double dark = tail_centroid(800.0);
  const double bright = tail_centroid(8000.0);
  REQUIRE(dark < 0.6 * bright);
}

TEST_CASE("the gate closes once the hold has run out below the threshold", "[reverb-character]") {
  DattorroReverbConfig config = wet_config();
  config.decay = 0.9f;
  const Render open = impulse_response(config, 2.0);
  const double open_peak = peak(open.left, 0.0, 2.0);

  const double threshold = 0.3 * open_peak;
  std::size_t first = 0;
  std::size_t last = 0;
  for (std::size_t i = 0; i < open.left.size(); ++i) {
    if (std::fabs(open.left[i]) >= threshold || std::fabs(open.right[i]) >= threshold) {
      if (first == 0) first = i;
      last = i;
    }
  }
  REQUIRE(last > first);

  constexpr double kHoldMs = 40.0;
  config.gate_threshold_db = static_cast<float>(20.0 * std::log10(threshold));
  config.gate_hold_ms = static_cast<float>(kHoldMs);
  const Render gated = impulse_response(config, 2.0);

  const double closed_from = static_cast<double>(last) / kRate + kHoldMs / 1000.0 + 0.03;
  REQUIRE(closed_from < 1.9);
  // The tail is still there without the gate and gone with it.
  REQUIRE(peak(open.left, closed_from, closed_from + 0.2) > 0.1 * threshold);
  REQUIRE(peak(gated.left, closed_from, 2.0) < 0.01 * threshold);
  REQUIRE(peak(gated.right, closed_from, 2.0) < 0.01 * threshold);

  // While it is open the gate passes the tail at unity.
  const double t0 = static_cast<double>(first) / kRate + 0.02;
  const double t1 = static_cast<double>(last) / kRate;
  REQUIRE(t1 > t0);
  REQUIRE(energy(gated.left, t0, t1) == Catch::Approx(energy(open.left, t0, t1)).epsilon(0.02));

  // A longer hold keeps the tail longer.
  config.gate_hold_ms = 400.0f;
  const Render held = impulse_response(config, 2.0);
  REQUIRE(peak(held.left, closed_from, closed_from + 0.2) > 0.05 * threshold);
}

TEST_CASE("the gate types shape the wet image over the hold", "[reverb-character]") {
  DattorroReverbConfig config = wet_config();
  config.decay = 0.9f;
  const Render open = impulse_response(config, 2.0);
  const double open_peak = peak(open.left, 0.0, 2.0);
  const double threshold = 0.03 * open_peak;
  std::size_t first = 0;
  std::size_t last = 0;
  for (std::size_t i = 0; i < open.left.size(); ++i) {
    if (std::fabs(open.left[i]) >= threshold || std::fabs(open.right[i]) >= threshold) {
      if (first == 0) first = i;
      last = i;
    }
  }
  constexpr double kHoldMs = 200.0;
  const double t_first = static_cast<double>(first) / kRate;
  const double hold_s = kHoldMs / 1000.0;
  REQUIRE(static_cast<double>(last) / kRate > t_first + 2.0 * hold_s);  // open long enough to sweep

  config.gate_threshold_db = static_cast<float>(20.0 * std::log10(threshold));
  config.gate_hold_ms = static_cast<float>(kHoldMs);
  config.gate_type = DattorroGateType::kNormal;
  const Render normal = impulse_response(config, 2.0);
  config.gate_type = DattorroGateType::kReverse;
  const Render reverse = impulse_response(config, 2.0);
  config.gate_type = DattorroGateType::kSweep1;
  const Render sweep1 = impulse_response(config, 2.0);
  config.gate_type = DattorroGateType::kSweep2;
  const Render sweep2 = impulse_response(config, 2.0);

  const double early_a = t_first + 0.005;
  const double early_b = t_first + 0.15 * hold_s;
  const double late_a = t_first + hold_s;
  const double late_b = t_first + 2.0 * hold_s;

  // Reverse swells: its early share of the normal gate's energy is below its late share.
  const double reverse_early =
      energy(reverse.left, early_a, early_b) / energy(normal.left, early_a, early_b);
  const double reverse_late =
      energy(reverse.left, late_a, late_b) / energy(normal.left, late_a, late_b);
  REQUIRE(reverse_early < 0.25);
  REQUIRE(reverse_late == Catch::Approx(1.0).epsilon(0.05));

  // Sweeps move the image from one side to the other, in opposite directions.
  REQUIRE(energy(sweep1.left, early_a, early_b) > 2.0 * energy(sweep1.right, early_a, early_b));
  REQUIRE(energy(sweep1.right, late_a, late_b) > 2.0 * energy(sweep1.left, late_a, late_b));
  REQUIRE(energy(sweep2.right, early_a, early_b) > 2.0 * energy(sweep2.left, early_a, early_b));
  REQUIRE(energy(sweep2.left, late_a, late_b) > 2.0 * energy(sweep2.right, late_a, late_b));
}

TEST_CASE("the reverb's automation ids are in place and refuse what they cannot take",
          "[reverb-character]") {
  DattorroReverb reverb(wet_config());
  reverb.prepare(kRate, kBlock);
  REQUIRE(reverb.set_parameter(5, 2500.0f));
  REQUIRE(reverb.set_parameter(6, -30.0f));
  REQUIRE(reverb.set_parameter(7, 80.0f));
  REQUIRE(reverb.set_parameter(8, 3.0f));
  REQUIRE_FALSE(reverb.set_parameter(8, 4.0f));
  REQUIRE_FALSE(reverb.set_parameter(8, 1.5f));
  REQUIRE_FALSE(reverb.set_parameter(8, -1.0f));
  for (unsigned int id = 5; id <= 8; ++id) REQUIRE(reverb.parameter_is_realtime_safe(id));

  REQUIRE(reverb.set_parameter(9, 12.0f));
  REQUIRE(reverb.set_parameter(10, 3.0f));
  REQUIRE(reverb.parameter_is_realtime_safe(9));
  REQUIRE(reverb.parameter_is_realtime_safe(10));
  bool has_pre_delay = false;
  bool has_character = false;
  for (const auto& d : reverb.parameter_descriptors()) {
    has_pre_delay = has_pre_delay || (d.key == std::string("preDelayMs") && d.id == 9);
    has_character = has_character || (d.key == std::string("character") && d.id == 10);
  }
  REQUIRE(has_pre_delay);
  REQUIRE(has_character);
  REQUIRE_FALSE(reverb.set_parameter(10, 1.5f));
  REQUIRE_FALSE(reverb.set_parameter(10, 7.0f));
}
