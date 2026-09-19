/// @file waveguide_bend_range_test.cpp
/// @brief How far down a waveguide engine can be bent before its delay line
///        runs out, and that running out is what the line length decides.
///
/// A waveguide's downward reach is bounded by the delay line it reads from, and
/// the clamp that enforces the bound saturates: the glide stops descending, the
/// note keeps sounding, and nothing anywhere reports it. That failure mode is
/// why this file measures at all -- a pitch that silently stops following is
/// indistinguishable from one that followed, in every instrument except the
/// pitch itself.
///
/// The measurement is travel rather than accuracy. An engine's sounding pitch
/// is not the requested one: a brass lip resonance drags the pitch back against
/// a bend by more than half of it, and every waveguide is tuned to within a few
/// cents rather than exactly. What a pinned line does instead is stop moving,
/// so what is required here is that the pitch is still descending at the deep
/// bend -- a property no engine's own tuning error can fake, and one a pinned
/// line cannot satisfy however well tuned it is.
///
/// The positive control is the second case: the same engine, the same note and
/// the same bend, driven at two delay-line lengths. It has to show a pin at the
/// short one, or nothing above is evidence that this file could report one.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "midi/synth/native_synth.h"
#include "midi/synth/plucked_string_voice.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::PluckedStringPatchParams;
using sonare::midi::synth::PluckedStringVoiceCore;
using sonare::midi::synth::SynthEngineMode;
using sonare::test::event;
using sonare::test::fft_fundamental;
using sonare::test::render_left;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
/// Middle C: high enough that every engine's slab holds several periods, low
/// enough that a bend stays well inside the fundamental's resolvable range.
constexpr uint8_t kNote = 60;
constexpr uint8_t kVelocity = 100;
constexpr double kNoteHz = 261.6255653;

/// Frames rendered before the analysis window, so the onset and the bore or
/// pipe speech are over, and frames the window itself spans.
constexpr int kSettleFrames = 8192;
constexpr int kWindowFrames = 16384;

/// Cents of further descent required between the two deep bends. With no lip
/// drag the interval itself is 200 cents; the brass lip gives back most of it,
/// and a line that has run out gives back all of it.
constexpr double kTravelFloor = 30.0;

double cents_between(double from_hz, double to_hz) { return 1200.0 * std::log2(to_hz / from_hz); }

/// Sounds @p note with a downward bend of @p cents and returns the measured
/// fundamental. The bend is set before the note starts, so the line is read at
/// the bent delay from the first sample and no glide transient sits inside the
/// window. A bend range of N semitones with the wheel at its bottom stop is
/// exactly -N * 100 cents, which is why the range carries the depth.
double bent_fundamental(SynthEngineMode mode, int cents) {
  NativeSynthConfig cfg;
  cfg.patch = NativeSynthPatch{};
  cfg.patch.mode = mode;
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.amp_env.sustain = 1.0f;
  cfg.patch.amp_env.release_ms = 100.0f;

  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  const uint8_t semitones = static_cast<uint8_t>(cents / 100);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 101, 0)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 100, 0)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 6, semitones)));
  synth.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 0)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity)));

  const std::vector<float> out = render_left(synth, kSettleFrames + kWindowFrames);
  const double hint = kNoteHz * std::exp2(-static_cast<double>(cents) / 1200.0);
  return fft_fundamental(out, static_cast<size_t>(kSettleFrames), hint);
}

struct WaveguideEngine {
  SynthEngineMode mode;
  const char* label;
};

/// Every engine that reads a note through a delay line. An engine missing from
/// here keeps a per-note span nobody measures, so the list is the claim and its
/// length is printed below rather than left to be counted by eye.
constexpr WaveguideEngine kEngines[] = {
    {SynthEngineMode::kPiano, "piano"},
    {SynthEngineMode::kKarplusStrong, "karplus-strong"},
    {SynthEngineMode::kPluckedString, "plucked string"},
    {SynthEngineMode::kHarpsichord, "harpsichord"},
    {SynthEngineMode::kBowedString, "bowed string"},
    {SynthEngineMode::kReed, "reed"},
    {SynthEngineMode::kBrass, "brass"},
    {SynthEngineMode::kFlute, "flute"},
    {SynthEngineMode::kPipeOrgan, "pipe organ"},
};

}  // namespace

TEST_CASE("a waveguide keeps descending past the span a note-on period would buy",
          "[midi][synth][waveguide]") {
  // A per-note span is 1.3x the period on a string and 1.15x in a pipe, which
  // at this note reaches 489 and 242 cents. Both bends below sit past the first
  // of those, so a line sized that way pins before the shallower one and the
  // interval between the two collapses to nothing.
  size_t measured = 0;
  for (const WaveguideEngine& engine : kEngines) {
    CAPTURE(engine.label);
    const double shallow = bent_fundamental(engine.mode, 500);
    const double deep = bent_fundamental(engine.mode, 700);
    REQUIRE(shallow > 0.0);
    REQUIRE(deep > 0.0);
    const double travel = -cents_between(shallow, deep);
    CAPTURE(shallow, deep, travel);
    REQUIRE(travel > kTravelFloor);
    ++measured;
  }
  // Un-reach is a failure rather than a clean run: an engine list that stopped
  // being iterated reports exactly what a passing tree reports.
  INFO("engines measured: " << measured);
  REQUIRE(measured == std::size(kEngines));
}

TEST_CASE("the delay line length is what bounds the descent", "[midi][synth][waveguide]") {
  // The same core, the same note and the same bend at two line lengths: the
  // slab the engine is allocated, and the span a note-on period used to cut it
  // to. Nothing else differs, so a pin at the short length is the line length
  // and cannot be the engine's tuning, its envelope or its excitation.
  const int slab = sonare::midi::synth::plucked_string_buffer_capacity(kRate);
  const double period = kRate / kNoteHz;
  const int note_span = static_cast<int>(period * 1.3f) + 8;
  REQUIRE(note_span < slab);

  auto descent_cents = [&](int capacity, double ratio) {
    std::vector<float> buffer(static_cast<size_t>(slab), 0.0f);
    PluckedStringVoiceCore core;
    core.attach(buffer.data(), capacity);
    core.start(PluckedStringPatchParams{}, kRate, kNote, kVelocity, 1u);
    std::vector<float> out(static_cast<size_t>(kSettleFrames + kWindowFrames));
    for (float& sample : out) sample = core.render(static_cast<float>(ratio));
    const double hint = kNoteHz * ratio;
    return cents_between(kNoteHz, fft_fundamental(out, static_cast<size_t>(kSettleFrames), hint));
  };

  // A seventh below: 700 cents of descent asked for on both arms.
  const double asked = -700.0;
  const double ratio = std::exp2(asked / 1200.0);
  const double on_slab = descent_cents(slab, ratio);
  const double on_note_span = descent_cents(note_span, ratio);
  CAPTURE(slab, note_span, on_slab, on_note_span);

  // The slab arm arrives; the note-span arm stops around 490 cents, which is
  // what a 1.3x span reaches once the stencil margin and the loop compensation
  // are taken out of it. The gap between the two is the whole point.
  REQUIRE(on_slab < asked + 40.0);
  REQUIRE(on_note_span > asked + 150.0);
  REQUIRE(on_note_span < -400.0);
}
