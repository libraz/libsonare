/// @file excitation_axis_test.cpp
/// @brief That a breath controller reaches the sound of every engine whose
///        excitation axes declare it does, and reaches nothing else.
///
/// The measurement is a control subtraction, because the patch's own envelope,
/// its body resonator and its drift LFO all move the level on their own: a
/// render whose RMS rose after a CC proves nothing until an identical render
/// that received no CC has been subtracted from it. Restricting the window to
/// the sustain is necessary but nowhere near sufficient -- the attack/hold/decay
/// times differ per patch, the approach is asymptotic so "past decay_ms" is not
/// "arrived", and the drift and unison beating keep moving inside the window.
///
/// Three walls, and a claim needs all three: the control-subtracted difference
/// must carry real energy (a pair of silent renders subtracts to a perfect zero
/// and would otherwise pass every ratio test), the spectrum must move rather
/// than only the gain, and the level must move in the direction the engine
/// declares.
///
/// What is deliberately NOT asserted is the direction the spectrum moves. A
/// waveguide here emits the pressure inside the bore rather than the field
/// radiated from its open end, and the two differ by a derivative, so opening
/// the reflection filter measurably darkens the bore even though it brightens
/// the instrument. Writing the measured sign into a test would pin that down as
/// intended. Which way a voice should move is a calibration question and the
/// ear settles it; this file settles only that the controller arrives.
///
/// The engines are addressed through their GM programs rather than through
/// hand-built patches, so what is measured is the bank a host actually hears.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "midi/synth/excitation_axes.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::synth::engine_axis_capability;
using sonare::midi::synth::kAxisBrightness;
using sonare::midi::synth::kAxisForce;
using sonare::midi::synth::kAxisNone;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::SynthEngineMode;
using sonare::test::event;
using sonare::test::render_left;
using sonare::test::rms;
using sonare::test::spectral_centroid;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kNote = 60;
constexpr int kVelocity = 100;

/// Frames rendered before the first controller step, so the onset transient and
/// the pipe/bore speech are over on both sides of the subtraction.
constexpr int kPrefillFrames = 16384;
/// Segments of the ramp, and the frames each one holds a controller position.
/// One segment is long enough to cover the 8 ms control glide many times over.
constexpr int kSegments = 8;
constexpr int kSegmentFrames = 4096;

/// Difference RMS a controller has to reach for the axis to count as arriving.
/// Two orders under the quietest voice measured here, so it separates "reached
/// the sound" from "reached nothing" without standing in for audibility.
constexpr double kReachFloor = 1.0e-4;
/// Centroid travel, as a fraction of its own mean, that says the controller
/// moved the spectrum rather than the output gain. The narrowest measured
/// response is the pipe organ's at about 0.06.
constexpr double kSpectrumFloor = 0.03;

/// What the engine's force axis does to the level. Declared per engine rather
/// than assumed uniform: two of them measurably do not raise it, and the reason
/// is the engine's own structure rather than the wiring under test. An
/// exclusion argued only in prose is invisible to anything mechanical, so it
/// rides on the entry.
enum class ForceLevel {
  /// More drive, more level, with the control's own contour subtracted out.
  kRises,
  /// The axis reaches the sound but the level is not where it lands.
  kElsewhere,
};

/// One engine and the GM program that voices it in the fallback bank.
struct EngineProbe {
  SynthEngineMode mode;
  int program;
  const char* label;
  ForceLevel force_level;
};

/// Every engine the capability table declares an axis for, at a program the
/// bank voices with it. Kept beside the table rather than derived from it: the
/// point of the probe is that a declared axis is audible, and a list generated
/// from the same declaration could not fail that way.
constexpr EngineProbe kProbes[] = {
    // No controller reaches the registration, so the morph axis is exercised by
    // the mod matrix alone and neither ramp below touches this row.
    {SynthEngineMode::kAdditive, 16, "additive (Drawbar Organ)", ForceLevel::kElsewhere},
    // Wind pressure sets the pipe's speech and its speaking frequency; what it
    // moves here is the tuning and the onset rather than the loudness, and the
    // level side of an organ's wind is the shared supply's sag, a bus stage.
    {SynthEngineMode::kPipeOrgan, 19, "pipe organ (Church Organ)", ForceLevel::kElsewhere},
    {SynthEngineMode::kFreeReed, 21, "free reed (Accordion)", ForceLevel::kRises},
    {SynthEngineMode::kBowedString, 40, "bowed string (Violin)", ForceLevel::kRises},
    {SynthEngineMode::kVocal, 52, "vocal (Choir Aahs)", ForceLevel::kElsewhere},
    {SynthEngineMode::kBrass, 56, "brass (Trumpet)", ForceLevel::kRises},
    {SynthEngineMode::kReed, 65, "reed (Alto Sax)", ForceLevel::kRises},
    // The jet holds its own limit cycle, so the loop gain rather than the
    // breath decides the amplitude and the breath is heard as colour.
    {SynthEngineMode::kFlute, 73, "flute (Flute)", ForceLevel::kElsewhere},
};

/// A struck or plucked engine, to show the same ramp reaches nothing there.
constexpr EngineProbe kDecliningProbes[] = {
    {SynthEngineMode::kPiano, 0, "piano (Acoustic Grand)", ForceLevel::kElsewhere},
    {SynthEngineMode::kKarplusStrong, 24, "karplus-strong (Nylon Guitar)", ForceLevel::kElsewhere},
    {SynthEngineMode::kModal, 11, "modal (Vibraphone)", ForceLevel::kElsewhere},
};

NativeSynth make_synth(int program) {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.polyphony = 4;
  cfg.bus_drive = 0.0f;
  cfg.dc_block = true;
  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  synth.on_event(
      0, event(sonare::midi::make_midi1_program_change(0, 0, static_cast<uint8_t>(program))));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity)));
  return synth;
}

/// What one ramp measured, per segment.
struct RampMeasurement {
  /// RMS of the control-subtracted difference signal.
  std::vector<double> difference_rms;
  /// RMS of the ramped render minus the RMS of the control's same segment: the
  /// level response with the patch's own contour removed.
  std::vector<double> level_delta;
  /// Spectral centroid of the ramped segment (Hz).
  std::vector<double> centroid;
};

/// Renders one synth with @p controller stepped from silence to full across the
/// segments, and an identical one that receives nothing, and reports the three
/// per-segment measures above.
RampMeasurement measure_ramp(int program, uint8_t controller) {
  NativeSynth ramped = make_synth(program);
  NativeSynth control = make_synth(program);

  render_left(ramped, kPrefillFrames);
  render_left(control, kPrefillFrames);

  RampMeasurement out;
  for (int seg = 0; seg < kSegments; ++seg) {
    const int value = ((seg + 1) * 127) / kSegments;
    ramped.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, controller,
                                                                     static_cast<uint8_t>(value))));
    const std::vector<float> a = render_left(ramped, kSegmentFrames);
    const std::vector<float> b = render_left(control, kSegmentFrames);

    std::vector<float> diff(a.size(), 0.0f);
    for (std::size_t i = 0; i < a.size(); ++i) diff[i] = a[i] - b[i];

    out.difference_rms.push_back(static_cast<double>(rms(diff)));
    out.level_delta.push_back(static_cast<double>(rms(a)) - static_cast<double>(rms(b)));
    out.centroid.push_back(spectral_centroid(a, 0));
  }
  return out;
}

/// Peak-to-peak travel of a series as a fraction of its mean. Signless on
/// purpose: it answers "did the controller move this" rather than "which way".
double spread(const std::vector<double>& values) {
  double lo = values.front();
  double hi = values.front();
  double sum = 0.0;
  for (const double v : values) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
    sum += v;
  }
  const double mean = std::abs(sum / static_cast<double>(values.size()));
  return mean > 0.0 ? (hi - lo) / mean : 0.0;
}

/// Largest of a series, for the reachability wall: a ramp that crosses the
/// patch's own setting subtracts to nearly nothing exactly there, so the
/// endpoints alone would understate what the controller reached.
double peak(const std::vector<double>& values) {
  double hi = values.front();
  for (const double v : values) hi = std::max(hi, v);
  return hi;
}

/// The whole series on one line, so a failure shows the shape rather than only
/// its endpoints.
std::string series(const std::vector<double>& values) {
  std::string out;
  for (const double v : values) {
    if (!out.empty()) out += ' ';
    out += std::to_string(v);
  }
  return out;
}

}  // namespace

TEST_CASE("a breath ramp reaches every engine that declares the force axis",
          "[midi][synth][excitation]") {
  for (const EngineProbe& probe : kProbes) {
    if ((engine_axis_capability(probe.mode).mask & kAxisForce) == 0u) continue;
    CAPTURE(probe.label);
    const RampMeasurement m = measure_ramp(probe.program, 2);

    // Wall 1: the difference signal carries energy. Two silent renders subtract
    // to an exact zero, which would satisfy every ratio below.
    CAPTURE(series(m.difference_rms));
    REQUIRE(peak(m.difference_rms) > kReachFloor);

    // Wall 2: it reached the spectrum, not just the output gain.
    CAPTURE(series(m.centroid));
    REQUIRE(spread(m.centroid) > kSpectrumFloor);

    // Wall 3: and the level moves the way this engine says it does. The two
    // that say otherwise carry their reason on their table entry.
    if (probe.force_level == ForceLevel::kRises) {
      CAPTURE(series(m.level_delta));
      REQUIRE(m.level_delta.back() > m.level_delta.front());
    }
  }
}

TEST_CASE("a brightness ramp reaches every engine that declares the brightness axis",
          "[midi][synth][excitation]") {
  for (const EngineProbe& probe : kProbes) {
    if ((engine_axis_capability(probe.mode).mask & kAxisBrightness) == 0u) continue;
    CAPTURE(probe.label);
    const RampMeasurement m = measure_ramp(probe.program, 74);
    CAPTURE(series(m.difference_rms));
    REQUIRE(peak(m.difference_rms) > kReachFloor);
    CAPTURE(series(m.centroid));
    REQUIRE(spread(m.centroid) > kSpectrumFloor);
  }
}

TEST_CASE("the same ramp reaches nothing in an engine that declares no axis",
          "[midi][synth][excitation]") {
  for (const EngineProbe& probe : kDecliningProbes) {
    REQUIRE(engine_axis_capability(probe.mode).mask == kAxisNone);
    CAPTURE(probe.label);
    // Exactly zero rather than under a floor: the controller never reaches a
    // setter, so the two renders are the same arithmetic on the same state.
    const RampMeasurement force = measure_ramp(probe.program, 2);
    const RampMeasurement bright = measure_ramp(probe.program, 74);
    for (int seg = 0; seg < kSegments; ++seg) {
      CAPTURE(seg);
      REQUIRE(force.difference_rms[static_cast<std::size_t>(seg)] == 0.0);
      REQUIRE(bright.difference_rms[static_cast<std::size_t>(seg)] == 0.0);
    }
  }
}

TEST_CASE("every engine mode declares whether it takes an excitation axis",
          "[midi][synth][excitation]") {
  int covered = 0;
  int excluded = 0;
  for (int i = 0; i <= sonare::midi::synth::kSynthEngineModeMax; ++i) {
    const auto cap = engine_axis_capability(static_cast<SynthEngineMode>(i));
    if (cap.mask == kAxisNone) {
      // A continuous engine with no axis is the defect the header's
      // static_assert exists to stop, so it cannot appear here either.
      REQUIRE_FALSE(cap.continuous);
      ++excluded;
    } else {
      ++covered;
    }
  }
  REQUIRE(covered + excluded == sonare::midi::synth::kSynthEngineModeMax + 1);
  // Every mode that owns an axis is probed above, so a mode added with an axis
  // and no probe fails here rather than going unmeasured.
  REQUIRE(covered == static_cast<int>(std::size(kProbes)));
}
