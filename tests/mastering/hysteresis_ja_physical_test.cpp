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
/// The quasi-static sweeps take one Euler step per sample by construction, so
/// they say nothing about the adaptive sub-stepping. The last three cases cover
/// that separately: the raw single-step engine under a fast drive, the sub-step
/// branch under the max_field_step the shipped stages set, and the regime past
/// the sub-step budget where the cap leaves each sub-step larger than asked for.
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
#include "util/constants.h"

namespace common = sonare::mastering::common;

namespace {

using Catch::Matchers::WithinRel;
using sonare::constants::kTwoPiD;

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

/// Largest field change between consecutive samples of the sinusoidal stimulus
/// used by the sub-step cases, which is what decides how many sub-steps
/// process() asks for.
double largest_field_step(double amplitude, double hz, double sample_rate, int samples) {
  double largest = 0.0;
  for (int i = 1; i < samples; ++i) {
    const double now = amplitude * std::sin(kTwoPiD * hz * static_cast<double>(i) / sample_rate);
    const double before =
        amplitude * std::sin(kTwoPiD * hz * static_cast<double>(i - 1) / sample_rate);
    largest = std::max(largest, std::abs(now - before));
  }
  return largest;
}

/// Peak magnetization over a sine drive at `hz`.
double peak_over_sine(const common::JilesAthertonConfig& config, double amplitude, double hz,
                      double sample_rate, int samples) {
  common::JilesAtherton engine(config);
  common::JilesAthertonState state;
  double peak = 0.0;
  for (int i = 0; i < samples; ++i) {
    const double field = amplitude * std::sin(kTwoPiD * hz * static_cast<double>(i) / sample_rate);
    peak = std::max(
        peak, std::abs(static_cast<double>(engine.process(state, static_cast<float>(field)))));
  }
  return peak;
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
// still has to leave the magnetization inside the envelope the model sets for
// itself. Ms is that envelope: the anhysteretic curve the trajectory chases is
// Ms*L(x) and approaches Ms only asymptotically, and neither branch may carry
// the magnetization across the curve within a step, so no step size can put the
// magnetization above Ms. The step size sets how close to it the drive gets, not
// whether the bound holds.
//
// The presets here leave max_field_step at its default 0, so this drives the raw
// single-step engine deliberately. It is not a statement about the shipped tape
// and transformer stages, which set that field and so sub-step a fast field
// change instead of integrating it in one go; those are covered below.
TEST_CASE("Jiles-Atherton magnetization stays inside its saturation envelope",
          "[mastering][saturation]") {
  constexpr double kFieldLimit = 40.0;
  constexpr int kSamples = 4000;

  // Each jump is several times every preset's coercivity, so a single Euler step
  // is outside the range where it tracks the loop, and the largest one crosses
  // most of the field range in a sample.
  for (double field_jump : {0.5, 2.0, 8.0, 32.0}) {
    for (const auto& preset : preset_loops()) {
      CAPTURE(preset.name, field_jump, field_jump / preset.config.coercivity);
      common::JilesAtherton engine(preset.config);
      common::JilesAthertonState state;

      double peak = 0.0;
      double field = 0.0;
      double direction = 1.0;
      for (int i = 0; i < kSamples; ++i) {
        field += direction * field_jump;
        if (field > kFieldLimit) direction = -1.0;
        if (field < -kFieldLimit) direction = 1.0;
        const double magnetization = engine.process(state, static_cast<float>(field));
        if (!(std::abs(magnetization) <= peak)) peak = std::abs(magnetization);
      }
      CAPTURE(peak);

      const double ms = preset.config.saturation_magnetization;
      REQUIRE(std::isfinite(peak));
      REQUIRE(peak <= ms);
      // The drive is fast enough that the envelope is approached rather than
      // passed by a wide margin, so the bound above is being exercised. Measured,
      // every row here lands between 0.992 and 0.998.
      REQUIRE(peak > 0.95 * ms);
    }
  }
}

// The sub-step branch, which the cases above never enter: they all leave
// max_field_step at zero, so process() takes exactly one step per sample and the
// loop that walks the field is dead code for them. Here every preset carries the
// max_field_step the shipped tape and transformer stages give it - their own
// coercivity - and the drive is fast enough that the branch runs.
//
// The two invariants are the ones that hold for any parameter set: the
// magnetization stays inside its own saturation envelope, and a level step still
// moves the output. The second is what a magnetization stuck against a bound
// loses first - a pinned stage returns the same peak for every level, so its
// step ratio is exactly 1 - and it is asserted rather than a peak value because
// the peaks themselves are preset-specific.
TEST_CASE("Jiles-Atherton sub-stepping holds the envelope and the level response",
          "[mastering][saturation]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kSamples = 48000;
  const double amplitudes[] = {0.25, 0.5, 1.0, 2.0, 4.0};

  for (const auto& preset : preset_loops()) {
    for (double hz : {1000.0, 6000.0, 12000.0, 16000.0}) {
      auto config = preset.config;
      config.max_field_step = config.coercivity;
      CAPTURE(preset.name, hz, config.max_field_step);

      const double ms = config.saturation_magnetization;
      double previous = 0.0;
      for (double amplitude : amplitudes) {
        const double peak = peak_over_sine(config, amplitude, hz, kSampleRate, kSamples);
        CAPTURE(amplitude, peak, previous);
        REQUIRE(peak <= ms);
        // Measured, the smallest step on this ladder is 1.049 at the top of it,
        // where the stage is deepest in compression.
        if (previous > 0.0) REQUIRE(peak > previous * 1.02);
        previous = peak;
      }

      // Non-vacuous on both counts: the branch under test was entered, and the
      // ladder reached the compressed part of the curve where a pinned stage
      // would stop responding rather than passing on the linear part.
      const double demand = largest_field_step(amplitudes[4], hz, kSampleRate, kSamples);
      CAPTURE(demand);
      REQUIRE(demand > static_cast<double>(config.max_field_step));
      REQUIRE(previous > 0.8 * ms);
    }
  }
}

// Past the sub-step budget. Asking for more sub-steps than kMaxSubSteps allows
// leaves each one larger than max_field_step requested, which is the regime the
// sub-stepping exists to avoid and which it cannot avoid here - so the envelope
// has to be held by the step itself rather than by the sub-step count. A 16 kHz
// drive at 48 kHz moves the field by up to 1.73 times its own amplitude between
// samples, so the top of the ladder here - a field amplitude of 16, which is
// what a full-scale input reaches at the +24 dB the tape stage documents - asks
// for five to sixteen times the budget depending on the preset.
TEST_CASE("Jiles-Atherton holds the envelope past the sub-step budget", "[mastering][saturation]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kSamples = 48000;
  constexpr double kHz = 16000.0;
  const double amplitudes[] = {1.0, 4.0, 16.0};

  for (const auto& preset : preset_loops()) {
    auto config = preset.config;
    config.max_field_step = config.coercivity;
    const double budget =
        common::JilesAtherton::kMaxSubSteps * static_cast<double>(config.max_field_step);
    const double demand = largest_field_step(amplitudes[2], kHz, kSampleRate, kSamples);
    CAPTURE(preset.name, budget, demand);
    // The budget really is exhausted, so the cap path is the one under test.
    REQUIRE(demand > budget);

    const double ms = config.saturation_magnetization;
    double previous = 0.0;
    for (double amplitude : amplitudes) {
      const double peak = peak_over_sine(config, amplitude, kHz, kSampleRate, kSamples);
      CAPTURE(amplitude, peak, previous);
      REQUIRE(peak <= ms);
      // Measured, the smallest step across this ladder is 1.041.
      if (previous > 0.0) REQUIRE(peak > previous * 1.01);
      previous = peak;
    }
  }
}
