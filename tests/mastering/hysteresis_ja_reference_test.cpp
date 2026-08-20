/// @file hysteresis_ja_reference_test.cpp
/// @brief Jiles-Atherton engine checked against closed-form limits of the
///        model, computed here from the equations rather than copied from the
///        implementation.
///
/// Coverage boundary, written down because "reference test" reads wider than
/// this is:
///   - Setting reversibility to 1 zeroes the irreversible numerator, so the
///     anhysteretic and susceptibility cases below do not exercise the field
///     direction, the delta_M gate, or the coercivity at all.
///   - The irreversible term has no closed form, so the hysteresis machinery
///     proper is reached only by the saturation bound, which constrains it
///     without pinning a value.
///   - None of this pins solver accuracy. It pins where the solver converges
///     and that it stays inside the physical envelope, not how closely it
///     tracks between the points checked.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/common/hysteresis_ja.h"
#include "util/constants.h"

namespace common = sonare::mastering::common;

namespace {

using sonare::constants::kTwoPiD;

constexpr double kMs = 1.0;
constexpr double kShape = 0.3;
constexpr double kCoercivity = 0.11;

/// Langevin function from its definition, coth(x) - 1/x, in double precision
/// and with no shared code with the engine.
double langevin_reference(double x) {
  if (std::abs(x) < 1e-6) return x / 3.0;
  return 1.0 / std::tanh(x) - 1.0 / x;
}

/// Anhysteretic magnetization for a held field: the solution of
/// M = Ms * L((H + alpha*M)/a), reached by fixed-point iteration. With
/// reversibility 1 the model reduces to exactly this curve.
double anhysteretic_reference(double field, double alpha) {
  double m = 0.0;
  for (int i = 0; i < 2000; ++i) {
    const double next = kMs * langevin_reference((field + alpha * m) / kShape);
    if (std::abs(next - m) < 1e-13) return next;
    m = next;
  }
  return m;
}

common::JilesAthertonConfig reversible_config(double alpha) {
  return {static_cast<float>(kMs), static_cast<float>(kShape), static_cast<float>(kCoercivity),
          static_cast<float>(alpha), 1.0f};
}

/// Drives one full symmetric triangle cycle 0 -> +amplitude -> -amplitude -> 0
/// at a constant per-sample field step and returns the magnetization at the end.
/// The constant step keeps every sample above the engine's held-field threshold,
/// so the rate-independent loop equation runs on every sample and the
/// after-effect relaxation never contributes to the result.
double run_symmetric_cycle(const common::JilesAtherton& engine, common::JilesAthertonState& state,
                           double amplitude, double step) {
  const int quarter = static_cast<int>(amplitude / step);
  double field = 0.0;
  for (int i = 0; i < quarter; ++i) {
    field += step;
    engine.process(state, static_cast<float>(field));
  }
  for (int i = 0; i < 2 * quarter; ++i) {
    field -= step;
    engine.process(state, static_cast<float>(field));
  }
  for (int i = 0; i < quarter; ++i) {
    field += step;
    engine.process(state, static_cast<float>(field));
  }
  return state.magnetization;
}

}  // namespace

// With reversibility 1 the loop collapses onto the anhysteretic curve, which is
// closed-form. The mean-field coupling is swept as well: at reversibility 1 the
// two published conventions for the mean-field correction on the reversible
// branch coincide, so this pins that term without depending on which is meant.
TEST_CASE("Fully reversible Jiles-Atherton follows the anhysteretic curve",
          "[mastering][saturation]") {
  // Measured optimum. Above this the engine's forward-Euler truncation error
  // dominates and grows with the step; below it the error stops falling and
  // settles near 1.5e-4, because the magnetization accumulator is a float and
  // a finer sweep only adds rounding. So the tolerance below is set by float
  // accumulation, not by the integrator, and cannot be tightened by sweeping
  // more finely.
  constexpr double kStep = 1e-4;
  constexpr double kMaxField = 3.0;

  for (double alpha : {0.0, 1.6e-3, 0.05}) {
    CAPTURE(alpha);
    common::JilesAtherton engine(reversible_config(alpha));
    common::JilesAthertonState state;

    double worst = 0.0;
    double worst_field = 0.0;
    for (double field = kStep; field < kMaxField; field += kStep) {
      const double actual = engine.process(state, static_cast<float>(field));
      const double expected = anhysteretic_reference(field, alpha);
      if (std::abs(actual - expected) > worst) {
        worst = std::abs(actual - expected);
        worst_field = field;
      }
    }
    CAPTURE(worst, worst_field);
    // This case pins the shape of the curve over its whole range - the Langevin
    // form, and the Ms and shape scalings - where a wrong term fails by orders
    // of magnitude. The mean-field term is pinned by the susceptibility case
    // below, which resolves it far more sharply than this tolerance can.
    REQUIRE(worst < 5e-4);
  }
}

// Initial susceptibility. L(x) -> x/3 as x -> 0, so the anhysteretic slope at
// the origin is Ms/(3a), and the mean field turns that into
// (Ms/3a) / (1 - alpha*Ms/(3a)).
TEST_CASE("Jiles-Atherton initial susceptibility matches the small-field limit",
          "[mastering][saturation]") {
  constexpr double kProbe = 1e-5;

  for (double alpha : {0.0, 1.6e-3}) {
    CAPTURE(alpha);
    common::JilesAtherton engine(reversible_config(alpha));
    common::JilesAthertonState state;

    const double slope = engine.process(state, static_cast<float>(kProbe)) / kProbe;
    const double anhysteretic_slope = kMs / (3.0 * kShape);
    const double expected = anhysteretic_slope / (1.0 - alpha * anhysteretic_slope);
    CAPTURE(slope, expected);
    REQUIRE(std::abs(slope - expected) / expected < 1e-5);
  }
}

// Ms is by definition the saturation magnetization and L(x) -> 1, so a solution
// of the model approaches Ms from below and never passes it. This is the one
// case here that constrains the full model with the irreversible branch live,
// and it is a property of the published equations rather than of our arithmetic.
//
// The bound does not depend on the step size. The engine sub-steps a large field
// change but caps the number of sub-steps, so a per-sample field change past
// kMaxSubSteps times max_field_step leaves each sub-step larger than asked for;
// the bound is held there by the step itself, which may not carry the
// magnetization across the anhysteretic curve. The largest field step swept
// below crosses that cap, and hysteresis_ja_physical_test.cpp drives the same
// regime harder.
// A rate-independent hysteresis loop is closed: once the trajectory has settled,
// a symmetric drive cycle returns the magnetization to where the cycle started.
// The state is a point on the loop, so repeating the same excursion has to land
// on the same point. This is a property of the equations rather than of any
// fitted constant, and it holds independently of the loop's shape.
//
// The first cycle from a virgin state does not close: it starts at M = 0, which
// is inside the loop rather than on it, and traces the initial magnetization
// curve out to the loop. The assertion therefore starts after a warm-up. Two
// warm-up cycles are used; measured, the trajectory is already on the limit loop
// after one, and the closure error from the second cycle on is zero in float
// across amplitude, step size, and reversibility. The tolerance is set well
// above that measured zero rather than at it, so the case does not turn into a
// bit-exactness assertion on float accumulation, and still sits three orders
// below the virgin-cycle offset it has to distinguish itself from.
//
// What this constrains is the structure that makes the branches mirror each
// other - the field-direction sense and the branch selection driven by it.
// Perturbing a fitted constant (coercivity, the reversible weight, the Langevin
// series) moves the loop but leaves it closed, so those are not covered here.
TEST_CASE("Jiles-Atherton settles onto a closed hysteresis loop", "[mastering][saturation]") {
  constexpr int kWarmupCycles = 2;
  constexpr int kMeasuredCycles = 4;
  constexpr double kClosureTolerance = 1e-5;

  auto config = reversible_config(1.6e-3);
  config.reversibility = 0.4f;
  config.max_field_step = static_cast<float>(kCoercivity);

  for (double amplitude : {0.5, 1.0, 4.0}) {
    for (double step : {0.0005, 0.002, 0.01}) {
      CAPTURE(amplitude, step);
      common::JilesAtherton engine(config);
      common::JilesAthertonState state;

      const double virgin_start = state.magnetization;
      const double after_first = run_symmetric_cycle(engine, state, amplitude, step);
      for (int i = 1; i < kWarmupCycles; ++i) run_symmetric_cycle(engine, state, amplitude, step);

      double worst = 0.0;
      for (int i = 0; i < kMeasuredCycles; ++i) {
        const double start = state.magnetization;
        const double end = run_symmetric_cycle(engine, state, amplitude, step);
        worst = std::max(worst, std::abs(end - start));
      }
      CAPTURE(worst, after_first);
      REQUIRE(worst < kClosureTolerance);

      // Closure is trivially satisfied by a magnetization pinned at zero, so pin
      // the loop open as well: the settled state carries remanence, and the
      // virgin cycle it started from did not close.
      REQUIRE(std::abs(state.magnetization) > 0.01);
      REQUIRE(std::abs(after_first - virgin_start) > kClosureTolerance);
    }
  }
}

TEST_CASE("Jiles-Atherton magnetization stays within saturation", "[mastering][saturation]") {
  auto config = reversible_config(1.6e-3);
  config.reversibility = 0.4f;
  config.max_field_step = static_cast<float>(kCoercivity);

  SECTION("sinusoidal drive across level and rate") {
    for (double amplitude : {0.5, 1.0, 4.0}) {
      for (double hz : {50.0, 1000.0, 8000.0}) {
        CAPTURE(amplitude, hz);
        common::JilesAtherton engine(config);
        common::JilesAthertonState state;
        double peak = 0.0;
        for (int i = 0; i < 48000; ++i) {
          const double field = amplitude * std::sin(kTwoPiD * hz * i / 48000.0);
          peak = std::max(
              peak,
              std::abs(static_cast<double>(engine.process(state, static_cast<float>(field)))));
        }
        CAPTURE(peak);
        REQUIRE(peak <= kMs);
      }
    }
  }

  SECTION("triangle drive at a fixed per-sample field step") {
    for (double field_step : {0.5, 2.0, 6.0}) {
      CAPTURE(field_step);
      common::JilesAtherton engine(config);
      common::JilesAthertonState state;
      double peak = 0.0;
      double field = 0.0;
      int direction = 1;
      for (int i = 0; i < 48000; ++i) {
        field += direction * field_step;
        if (field > 20.0) direction = -1;
        if (field < -20.0) direction = 1;
        peak = std::max(
            peak, std::abs(static_cast<double>(engine.process(state, static_cast<float>(field)))));
      }
      CAPTURE(peak);
      REQUIRE(peak <= kMs);
    }
  }
}
