#pragma once

/// @file articulation_mode.h
/// @brief What a channel does with a note-on while another note on it is still
///        held.
///
/// Layering: depends on nothing. It names the channel's rule and never an
/// engine — whether a given engine can actually be carried into a new note is a
/// synthesis question and is declared with the engines (synth/articulation.h).
/// Split so midi/instrument.h can state the contract without depending on the
/// synth, the same way controller_profile.h names axes without naming engines.

#include <cstdint>

namespace sonare::midi {

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

/// Values in ArticulationMode, so a surface can refuse an ordinal instead of
/// casting one it does not recognise.
inline constexpr uint8_t kArticulationModeCount = 3;

}  // namespace sonare::midi
