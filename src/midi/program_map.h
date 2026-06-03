#pragma once

/// @file program_map.h
/// @brief Program change / bank select handling plus General MIDI (GM) and
///        General MIDI 2 (GM2) instrument name maps, the GM percussion (drum)
///        map, and CC / per-note controller naming/lookup.
///
/// Layering: this header depends ONLY on midi/ump (for the channel-voice
/// constructors). It is CONTROL-thread data: all lookups are pure table reads
/// and never allocate, but the maps are intended for editor / inspector use, not
/// the audio path. Out-of-range queries return a safe default (empty string view
/// or a "no match" sentinel), never a crash.
///
/// Bank select convention (MIDI 1.0): a program is fully identified by the pair
/// (bank, program) where bank is a 14-bit value carried as CC#0 (Bank Select
/// MSB) + CC#32 (Bank Select LSB) followed by a Program Change. GM2 uses
/// bank MSB 0x78 for the percussion bank and 0x79 for the melodic bank.

#include <array>
#include <cstdint>
#include <string_view>

#include "midi/ump.h"

namespace sonare::midi {

/// A fully-qualified MIDI program selection: a 14-bit bank (MSB/LSB combined)
/// plus a 7-bit program number. Trivially copyable value data.
struct ProgramSelection {
  uint8_t bank_msb = 0;  ///< CC#0 value (0..127).
  uint8_t bank_lsb = 0;  ///< CC#32 value (0..127).
  uint8_t program = 0;   ///< Program Change value (0..127).

  /// Combined 14-bit bank number (MSB << 7 | LSB).
  uint16_t bank() const noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(bank_msb & 0x7Fu) << 7) |
                                 static_cast<uint16_t>(bank_lsb & 0x7Fu));
  }

  bool operator==(const ProgramSelection& o) const noexcept {
    return bank_msb == o.bank_msb && bank_lsb == o.bank_lsb && program == o.program;
  }
  bool operator!=(const ProgramSelection& o) const noexcept { return !(*this == o); }
};

/// GM2 bank MSB constants (the only bank MSBs GM2 assigns special meaning).
enum class Gm2Bank : uint8_t {
  kMelodic = 0x79,     ///< Bank MSB selecting the melodic GM2 sound set.
  kPercussion = 0x78,  ///< Bank MSB selecting the percussion GM2 sound set.
};

/// The channel on which GM/GM2 percussion always plays (1-based channel 10,
/// i.e. zero-based channel index 9).
constexpr uint8_t kDrumChannelIndex = 9;

// ===========================================================================
// General MIDI instrument names
// ===========================================================================

/// Returns the GM Level 1 instrument name for a 0-based program number
/// (0..127). Out-of-range programs return an empty string view. The names match
/// the General MIDI Level 1 instrument list (128 programs, 16 families).
std::string_view gm_instrument_name(uint8_t program) noexcept;

/// Reverse lookup: returns the 0-based GM program number for an exact
/// (case-sensitive) instrument name, or -1 if no GM instrument has that name.
int gm_program_for_name(std::string_view name) noexcept;

/// Returns the 0-based program number of the first instrument in each of the
/// 16 GM families (Piano, Chromatic Percussion, Organ, Guitar, Bass, Strings,
/// Ensemble, Brass, Reed, Pipe, Synth Lead, Synth Pad, Synth Effects, Ethnic,
/// Percussive, Sound Effects). `family` is 0..15; out-of-range returns -1.
int gm_family_first_program(uint8_t family) noexcept;

/// Returns the human-readable GM family name for `family` (0..15), or empty.
std::string_view gm_family_name(uint8_t family) noexcept;

// ===========================================================================
// General MIDI 2 instrument names
// ===========================================================================

/// Returns the GM2 instrument name for a (bank_lsb, program) pair within the
/// GM2 melodic bank (bank MSB 0x79). GM2 keeps the 128 GM Level 1 names at bank
/// LSB 0 and adds variation banks at higher LSBs; for the variations covered by
/// this table the variation name is returned, otherwise it falls back to the
/// base GM name for that program. Out-of-range program returns empty.
std::string_view gm2_instrument_name(uint8_t bank_lsb, uint8_t program) noexcept;

// ===========================================================================
// General MIDI percussion (drum) map — channel 10
// ===========================================================================

/// Returns the GM percussion name for a note number on the drum channel. The GM
/// standard assigns names to notes 35..81 (Acoustic Bass Drum .. Open Triangle);
/// notes outside that range return an empty string view.
std::string_view gm_drum_name(uint8_t note) noexcept;

/// Reverse lookup: returns the drum note number for an exact percussion name, or
/// -1 if no GM percussion entry has that name.
int gm_drum_note_for_name(std::string_view name) noexcept;

// ===========================================================================
// Control change (CC) names
// ===========================================================================

/// Returns the standard MIDI controller name for a CC index (0..127). Defined
/// controllers (Bank Select, Modulation, Volume, Pan, Expression, Sustain, the
/// RPN/NRPN selectors, the channel-mode messages, ...) return their canonical
/// name; undefined controllers and out-of-range indices return an empty view.
std::string_view cc_name(uint8_t controller) noexcept;

/// Reverse lookup: returns the CC index for an exact controller name, or -1.
int cc_index_for_name(std::string_view name) noexcept;

// ===========================================================================
// MIDI 2.0 per-note / registered controller names
// ===========================================================================

/// Returns the name of a MIDI 2.0 registered per-note controller index
/// (per the MIDI 2.0 spec: 1 = Modulation, 2 = Breath, 3 = Pitch 7.25, 7 =
/// Volume, 8 = Balance, 10 = Pan, 11 = Expression, ...). Unassigned indices and
/// out-of-range values return an empty string view.
std::string_view per_note_controller_name(uint8_t index) noexcept;

// ===========================================================================
// Bank / program forward + round-trip helpers
// ===========================================================================

/// A bank/program change lowered to its three MIDI 1.0 channel-voice messages
/// (Bank Select MSB, Bank Select LSB, Program Change). Fixed-size POD; safe to
/// build on the control thread and hand to the RT path.
struct BankProgramMessages {
  std::array<Ump, 3> messages{};
  uint8_t count = 0;  ///< Number of populated messages (always 3 here).
};

/// Lowers a ProgramSelection to the three UMP messages (CC#0, CC#32, Program
/// Change) on `group`/`channel`. Pure; no allocation.
BankProgramMessages program_to_messages(uint8_t group, uint8_t channel,
                                        const ProgramSelection& selection) noexcept;

/// Tracks bank select / program change state for a single channel, so a stream
/// of CC#0 / CC#32 / Program Change UMPs can be folded back into the current
/// ProgramSelection (the round-trip inverse of program_to_messages). Control
/// thread; trivially copyable.
struct ProgramState {
  uint8_t pending_bank_msb = 0;
  uint8_t pending_bank_lsb = 0;
  ProgramSelection current{};
  bool program_seen = false;

  /// Folds one channel-voice UMP into the state. Bank Select CCs update the
  /// pending bank; a Program Change commits (pending bank, program) into
  /// `current`. Other messages are ignored. Returns true if `current` changed.
  bool observe(const Ump& ump) noexcept;
};

}  // namespace sonare::midi
