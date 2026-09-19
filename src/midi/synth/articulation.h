#pragma once

/// @file articulation.h
/// @brief How a channel treats a second note-on while the first is still held,
///        and which engines can carry a sounding note into a new pitch.
///
/// Legato here is voice continuation rather than an envelope shape: the voice
/// keeps its exciter, its delay line and its amplitude envelope, and only its
/// pitch moves. That is what a wind player's slur is, and it is also the only
/// form that reaches every engine — the per-sample pitch factor is the one
/// argument every engine's render() already takes, so nothing engine-specific
/// has to be re-derived to change the note.
///
/// Which engines can do it is declared, never inferred, on the same terms as
/// excitation_axes.h: one row per SynthEngineMode, a switch with no `default:`
/// so a new engine cannot be added without a decision, and a static_assert on
/// the pairing the row must satisfy.
///
/// The second half is a reach question, and it is why the row carries a floor.
/// A waveguide reads its delay line at the new note's period, so a note below
/// what the line can hold does not fail — it pins, silently, at the lowest
/// pitch the line reaches while the note keeps sounding. Refusing the
/// continuation instead costs an articulation and sounds the note correctly,
/// which is the direction to be wrong in.

#include <cstdint>

#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/brass_voice.h"
#include "midi/synth/excitation_axes.h"
#include "midi/synth/flute_voice.h"
#include "midi/synth/pipe_organ_voice.h"
#include "midi/synth/reed_voice.h"

namespace sonare::midi::synth {

/// What a channel does with a note-on while another note on the same channel is
/// still held.
enum class ArticulationMode : uint8_t {
  /// Every note-on takes its own voice.
  kPoly = 0,
  /// One note at a time; a new note-on stops the previous note and starts over.
  /// This is what GS MONO MODE and CC126 mean, and it is all they can reach.
  kMonoRetrigger = 1,
  /// One note at a time, carried: a new note-on re-tunes the sounding voice
  /// instead of starting one, so the exciter and the envelope never restart.
  /// Unreachable from a GS file by design — it is not a mode that standard
  /// names, and reading CC126 as this one would change what a compliant file
  /// sounds like.
  kMonoLegato = 2,
};

/// Whether an engine can be carried into a new note, and how low it reaches.
struct EngineLegato {
  /// The engine's sound generation survives a pitch change mid-note. False for
  /// a struck or plucked exciter: its energy is delivered once, so continuing
  /// the voice would slur into a note that was never struck and decay from the
  /// first note's remaining energy rather than sounding.
  bool continues;
  /// Lowest fundamental the engine's delay line can hold, in Hz at the 8'
  /// pitch. 0 means the engine has no delay line and no bound — an oscillator
  /// or a formant filter is retuned by arithmetic alone. Taken from the same
  /// constant the slab capacity is computed from, so the two cannot drift.
  float lowest_hz;
};

/// The accept set, one row per mode. No `default:` label: the exhaustiveness
/// warning is the whole mechanism.
constexpr EngineLegato engine_legato(SynthEngineMode mode) noexcept {
  switch (mode) {
    // Struck or plucked: the exciter is spent before the second sample. Also
    // the drawbar organ, which has no legato of its own -- a tonewheel is
    // always turning and a key only connects it, so there is nothing to carry.
    case SynthEngineMode::kKarplusStrong:
    case SynthEngineMode::kModal:
    case SynthEngineMode::kAdditive:
    case SynthEngineMode::kPercussion:
    case SynthEngineMode::kPiano:
    case SynthEngineMode::kPluckedString:
    case SynthEngineMode::kHarpsichord:
    // A sample is a recording of one note; re-pitching it far is a different
    // defect from the one this file avoids.
    case SynthEngineMode::kSample:
      return {false, 0.0f};
    // No delay line: the pitch is a number these read every sample.
    case SynthEngineMode::kSubtractive:
    case SynthEngineMode::kFm:
    case SynthEngineMode::kVocal:
    case SynthEngineMode::kFreeReed:
      return {true, 0.0f};
    case SynthEngineMode::kPipeOrgan:
      return {true, kPipeMinFundamentalHz};
    case SynthEngineMode::kBowedString:
      return {true, kBowedMinFundamentalHz};
    case SynthEngineMode::kReed:
      return {true, kReedMinFundamentalHz};
    case SynthEngineMode::kBrass:
      return {true, kBrassMinFundamentalHz};
    case SynthEngineMode::kFlute:
      return {true, kFluteMinFundamentalHz / kFluteBoreLengthPeriods};
  }
  return {false, 0.0f};
}

/// Whether a voice sounding @p from_note can be carried to @p to_note.
///
/// @p lowest_pitch_mult is the deepest pitch multiplier the voice actually
/// sounds, for an engine that voices a note at several pitches at once: a pipe
/// organ's 16' rank sounds an octave below the key, so it runs out of delay
/// line an octave before the 8' rank does. 1.0 for every other engine, and the
/// caller supplies it because the registration is the patch's, not the mode's.
bool accepts_legato(SynthEngineMode mode, uint8_t from_note, uint8_t to_note,
                    float lowest_pitch_mult = 1.0f) noexcept;

namespace articulation_detail {

/// A floor on a row that declines legato is a value nothing can ever read, so
/// it states a reach the engine is never asked about — the half-edited row.
/// That a floor is PRESENT wherever one is needed cannot be settled here,
/// because having a delay line is not something a row declares; the test file
/// settles it against the capacity functions themselves, which is the only
/// place the two numbers can be compared rather than restated.
constexpr bool no_row_declares_an_unreadable_floor() {
  for (int i = 0; i <= kSynthEngineModeMax; ++i) {
    const EngineLegato row = engine_legato(static_cast<SynthEngineMode>(i));
    if (!row.continues && row.lowest_hz != 0.0f) return false;
  }
  return true;
}

}  // namespace articulation_detail

static_assert(articulation_detail::no_row_declares_an_unreadable_floor(),
              "an engine that declines legato must not also declare how low it slurs");

}  // namespace sonare::midi::synth
