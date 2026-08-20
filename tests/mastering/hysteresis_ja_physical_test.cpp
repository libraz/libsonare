/// @file hysteresis_ja_physical_test.cpp
/// @brief Jiles-Atherton engine constrained by hysteresis quantities read back
///        from its own output.
///
/// Coercivity, remanence, saturation level, loop area and initial
/// susceptibility are measured by driving the engine with a quasi-static field
/// sweep and looking only at the magnetization it returns. None of them is
/// computed from a model constant, so each one moves when a constant moves -
/// including when the same edit is applied consistently everywhere in the
/// model. The two structural invariants at the end - positive differential
/// susceptibility, and a bounded magnetization envelope - are properties of the
/// loop equation that hold for any parameter set.
///
/// Coverage boundary:
///   - The mean-field coupling is a weak parameter at these presets. Doubling
///     it moves every quantity here by under half a percent, so it is not
///     constrained by this file; it is pinned by the closed-form susceptibility
///     case in hysteresis_ja_reference_test.cpp.
///   - The small-argument Langevin series is entered only for |x| < 1e-4, where
///     its quadratic correction term is around 1e-10 relative. The leading 1/3
///     term is pinned by the initial susceptibility below; the correction term
///     is below float resolution at every reachable input and is not pinned by
///     anything.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/common/hysteresis_ja.h"

namespace common = sonare::mastering::common;

namespace {

using Catch::Matchers::WithinRel;

/// Per-sample field increment of the quasi-static sweep. It is far below every
/// preset's coercivity, so the forward-Euler integration follows the loop
/// rather than slewing across it; halving it moves the quantities below by
/// under 0.2%.
constexpr double kFieldStep = 2e-4;

/// Field amplitude that drives every preset into saturation.
constexpr double kSaturatingAmplitude = 3.0;

/// Field amplitude that stays on the steep part of the anhysteretic curve, so
/// the loop is traced without the branches converging onto the saturation
/// asymptote.
constexpr double kSubSaturatingAmplitude = 0.8;

struct LoopMeasurement {
  /// Field magnitude at which the descending branch crosses M = 0.
  double coercivity = 0.0;
  /// Magnetization left at H = 0 on the descending branch.
  double remanence = 0.0;
  /// Largest |M| reached over the cycle.
  double saturation = 0.0;
  /// Enclosed loop area, the closed integral of M dH over one cycle.
  double area = 0.0;
  /// Total magnetization travelled in the direction opposite to the field over
  /// the cycle.
  double reverse_travel = 0.0;
};

/// Traces one symmetric field cycle at `amplitude` and reads the loop back off
/// the returned magnetization.
///
/// The sweep runs the initial magnetization curve out from the demagnetized
/// state and one full cycle before anything is recorded, so the measured cycle
/// starts on the limit loop rather than inside it. Every sample carries a field
/// change of kFieldStep, which is above the engine's held-field threshold, and
/// the two-argument process() supplies no sample rate, so the rate-independent
/// loop equation runs on every sample and the after-effect relaxation never
/// contributes.
LoopMeasurement measure_loop(const common::JilesAthertonConfig& config, double amplitude) {
  common::JilesAtherton engine(config);
  common::JilesAthertonState state;
  const int quarter = static_cast<int>(std::llround(amplitude / kFieldStep));

  LoopMeasurement measured;
  double previous_field = 0.0;
  double previous_magnetization = 0.0;
  bool recording = false;
  bool descending = false;

  // Each sample is computed from the ramp's own start field rather than
  // accumulated, so the sweep lands on its turning points exactly.
  auto ramp = [&](double from, int count, double direction) {
    for (int i = 1; i <= count; ++i) {
      const double field = from + direction * kFieldStep * static_cast<double>(i);
      const double magnetization = engine.process(state, static_cast<float>(field));
      if (recording) {
        measured.area += 0.5 * (magnetization + previous_magnetization) * (field - previous_field);
        measured.saturation = std::max(measured.saturation, std::abs(magnetization));
        const double step = magnetization - previous_magnetization;
        const double against = direction > 0.0 ? -step : step;
        if (against > 0.0) measured.reverse_travel += against;
        if (descending && previous_magnetization > 0.0 && magnetization <= 0.0) {
          const double t = previous_magnetization / (previous_magnetization - magnetization);
          measured.coercivity = std::abs(previous_field + t * (field - previous_field));
        }
        if (descending && previous_field > 0.0 && field <= 0.0) {
          const double t = previous_field / (previous_field - field);
          measured.remanence =
              previous_magnetization + t * (magnetization - previous_magnetization);
        }
      }
      previous_field = field;
      previous_magnetization = magnetization;
    }
  };

  ramp(0.0, quarter, 1.0);
  ramp(amplitude, 2 * quarter, -1.0);
  ramp(-amplitude, 2 * quarter, 1.0);

  recording = true;
  descending = true;
  previous_field = amplitude;
  previous_magnetization = state.magnetization;
  ramp(amplitude, 2 * quarter, -1.0);
  descending = false;
  ramp(-amplitude, 2 * quarter, 1.0);

  measured.area = std::abs(measured.area);
  return measured;
}

/// Initial susceptibility: the slope of the magnetization curve at the origin,
/// taken from the demagnetized state so the irreversible term starts at zero
/// and the slope is the model's reversible response to a vanishing field.
double initial_susceptibility(const common::JilesAthertonConfig& config) {
  constexpr double kProbeField = 1e-5;
  common::JilesAtherton engine(config);
  common::JilesAthertonState state;
  return engine.process(state, static_cast<float>(kProbeField)) / kProbeField;
}

struct PresetLoop {
  const char* name;
  common::JilesAthertonConfig config;
  double coercivity;
  double remanence;
  double saturation;
  double area;
  double susceptibility;
};

std::vector<PresetLoop> preset_loops() {
  return {
      {"oxide tape", common::jiles_atherton_presets::oxide_tape(), 0.058739, 0.064179, 0.897908,
       0.214260, 0.445347},
      {"silicon steel", common::jiles_atherton_presets::silicon_steel(), 0.066061, 0.095333,
       0.924733, 0.264561, 0.835961},
      {"mu metal", common::jiles_atherton_presets::mu_metal(), 0.014895, 0.027646, 0.939737,
       0.057085, 1.340620},
  };
}

}  // namespace

// The four quantities that define a hysteresis loop, measured off the loop the
// engine actually traces. The tolerances are set from how far a ten-percent
// move of a single model constant shifts each quantity: coercivity, remanence
// and area all move by six to ten percent under such a move, so a four-percent
// window separates the model as configured from a perturbed one while leaving
// far more room than the sweep resolution needs. The saturation level responds
// only to the saturation magnetization itself and gets a tighter window because
// nothing else pushes it around. The initial susceptibility gets the tightest
// window of all: it is a single evaluation from the demagnetized state with no
// accumulation behind it, so it resolves a small move in the leading Langevin
// term that the swept quantities average away.
TEST_CASE("Jiles-Atherton presets reproduce their measured hysteresis loops",
          "[mastering][saturation]") {
  for (const auto& preset : preset_loops()) {
    CAPTURE(preset.name);
    const LoopMeasurement loop = measure_loop(preset.config, kSaturatingAmplitude);
    CAPTURE(loop.coercivity, loop.remanence, loop.saturation, loop.area);

    REQUIRE_THAT(loop.coercivity, WithinRel(preset.coercivity, 0.04));
    REQUIRE_THAT(loop.remanence, WithinRel(preset.remanence, 0.04));
    REQUIRE_THAT(loop.saturation, WithinRel(preset.saturation, 0.02));
    REQUIRE_THAT(loop.area, WithinRel(preset.area, 0.04));

    // A loop, not a curve: it encloses area, it keeps magnetization at zero
    // field, and it stays under the saturation magnetization it is approaching.
    REQUIRE(loop.area > 0.0);
    REQUIRE(loop.remanence > 0.0);
    REQUIRE(loop.remanence < loop.saturation);
    REQUIRE(loop.saturation < preset.config.saturation_magnetization);

    const double susceptibility = initial_susceptibility(preset.config);
    CAPTURE(susceptibility);
    REQUIRE_THAT(susceptibility, WithinRel(preset.susceptibility, 0.01));
  }
}

// Loop area at a drive that does not reach saturation. The saturating loop is
// dominated by the flat part of the anhysteretic curve, which makes it almost
// blind to the anhysteretic shape parameter; measured on a sub-saturating loop
// the same quantity moves by nearly six percent for a ten-percent shape change.
TEST_CASE("Jiles-Atherton loop area holds below saturation", "[mastering][saturation]") {
  const double expected[] = {0.136948, 0.177436, 0.045845};
  const auto presets = preset_loops();
  for (size_t i = 0; i < presets.size(); ++i) {
    CAPTURE(presets[i].name);
    const LoopMeasurement loop = measure_loop(presets[i].config, kSubSaturatingAmplitude);
    CAPTURE(loop.area, loop.saturation);
    REQUIRE_THAT(loop.area, WithinRel(expected[i], 0.04));
    // The drive really did stay off the asymptote, so the reading above is the
    // sub-saturating loop and not a second copy of the major one.
    REQUIRE(loop.saturation < 0.8 * presets[i].config.saturation_magnetization);
  }
}

// Differential susceptibility is positive everywhere on the loop: while the
// drive field moves in one direction the magnetization never moves in the
// other. This holds for any parameter set, including at the branch reversals
// where the trajectory sits on the far side of the anhysteretic curve from the
// direction it is being driven, and it is what keeps the loop single-valued on
// each branch.
TEST_CASE("Jiles-Atherton magnetization never moves against the drive field",
          "[mastering][saturation]") {
  for (double amplitude : {kSubSaturatingAmplitude, kSaturatingAmplitude}) {
    for (const auto& preset : preset_loops()) {
      CAPTURE(preset.name, amplitude);
      const LoopMeasurement loop = measure_loop(preset.config, amplitude);
      CAPTURE(loop.reverse_travel);
      // Measured, this is exactly zero. The bound is set well above that so
      // the case does not become a bit-exactness assertion on float
      // accumulation, and far below the excursion a reversal produces when the
      // branch gate lets the irreversible term pull the magnetization back
      // toward the anhysteretic curve.
      REQUIRE(loop.reverse_travel < 1e-6);
      // Non-vacuous only if the sweep moved the magnetization at all.
      REQUIRE(loop.saturation > 0.1);
    }
  }
}

// A drive that slews faster than the loop equation can integrate in one step
// pushes the magnetization past the anhysteretic curve it is chasing. The model
// still has to stay inside a finite envelope set by its own saturation
// magnetization, rather than running away with the step size.
//
// The presets here leave max_field_step at its default 0, so this drives the raw
// single-step engine deliberately. It is not a statement about the shipped tape
// and transformer stages, which set that field and so sub-step a fast field
// change instead of integrating it in one go.
TEST_CASE("Jiles-Atherton magnetization stays inside its saturation envelope",
          "[mastering][saturation]") {
  constexpr double kFieldJump = 2.0;
  constexpr double kFieldLimit = 40.0;
  constexpr int kSamples = 4000;

  for (const auto& preset : preset_loops()) {
    CAPTURE(preset.name);
    common::JilesAtherton engine(preset.config);
    common::JilesAthertonState state;

    double peak = 0.0;
    double field = 0.0;
    double direction = 1.0;
    for (int i = 0; i < kSamples; ++i) {
      field += direction * kFieldJump;
      if (field > kFieldLimit) direction = -1.0;
      if (field < -kFieldLimit) direction = 1.0;
      const double magnetization = engine.process(state, static_cast<float>(field));
      if (!(std::abs(magnetization) <= peak)) peak = std::abs(magnetization);
    }
    CAPTURE(peak);

    const double ms = preset.config.saturation_magnetization;
    REQUIRE(std::isfinite(peak));
    REQUIRE(peak <= 1.2 * ms + 1e-5);
    // The drive is fast enough that the envelope is actually reached, so the
    // bound above is being exercised rather than passed by a wide margin.
    REQUIRE(peak > ms);
  }
}
