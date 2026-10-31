/// @file singing_pitch_compensation_test.cpp
/// @brief The drift detector for singing_pitch_compensation.inc: the table's
///        four corners are re-measured on the assembly the engine renders, so a
///        model change that stales the table, or a hand edit to it, goes red.
///
/// The fast case renders each corner at the tension the table stores and needs
/// the written pitch back; the slow case re-runs the generator's own fit there
/// and needs the stored tension back. Both go through tools/singing/pitch_fit,
/// the code the generator's sweep runs.

#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "midi/synth/singing_voice.h"
#include "singing/pitch_fit.h"

namespace sonare::midi::synth {
#include "midi/synth/singing_pitch_compensation.inc"
}  // namespace sonare::midi::synth

namespace {

using sonare::midi::synth::kSingingCompForce;
using sonare::midi::synth::kSingingCompForceCount;
using sonare::midi::synth::kSingingCompPitchCount;
using sonare::midi::synth::kSingingCompPitchHz;
using sonare::midi::synth::kSingingCompTension;

struct Corner {
  int force;
  int pitch;
};

constexpr Corner kCorners[] = {{0, 0},
                               {0, kSingingCompPitchCount - 1},
                               {kSingingCompForceCount - 1, 0},
                               {kSingingCompForceCount - 1, kSingingCompPitchCount - 1}};

/// Cents a corner may sit off its pitch: the fit's own resolution. A one per
/// cent edit to a stored tension moves F0 by about four.
constexpr double kCornerCents = 2.0;

}  // namespace

TEST_CASE("singing compensation table sounds its pitch at the four corners",
          "[midi][synth][singing]") {
  for (const Corner& c : kCorners) {
    const float hz = kSingingCompPitchHz[c.pitch];
    const float force = kSingingCompForce[c.force];
    const float tension = kSingingCompTension[c.force][c.pitch];
    const double f = sonare::tools::singing::assembly_f0(hz, force, tension);
    CAPTURE(hz, force, tension, f);
    REQUIRE(f > 0.0);
    REQUIRE(std::fabs(1200.0 * std::log2(f / hz)) <= kCornerCents);
  }
}

TEST_CASE("singing compensation table re-derives at the four corners",
          "[.][slow][midi][synth][singing]") {
  for (const Corner& c : kCorners) {
    const float hz = kSingingCompPitchHz[c.pitch];
    const float force = kSingingCompForce[c.force];
    const float stored = kSingingCompTension[c.force][c.pitch];
    const float fitted = sonare::tools::singing::fit_tension(hz, force, stored);
    CAPTURE(hz, force, stored, fitted);
    REQUIRE(fitted > 0.0f);
    // Inside one step of the fit's own scan, 0.4 % of tension.
    REQUIRE(std::fabs(fitted / stored - 1.0f) <= 4.0e-3f);
  }
}
