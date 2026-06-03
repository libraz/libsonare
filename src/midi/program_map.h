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

/// The nine GM2 percussion sets, identified by the bank LSB that selects them on
/// the GM2 percussion bank (bank MSB 0x78). The Standard set is the GM Level 1
/// drum map; the other sets re-voice a handful of notes (the rest stay Standard).
enum class Gm2DrumSet : uint8_t {
  kStandard = 0,
  kRoom = 8,
  kPower = 16,
  kElectronic = 24,
  kAnalog = 25,
  kJazz = 32,
  kBrush = 40,
  kOrchestra = 48,
  kSfx = 56,
};

/// Returns the GM2 percussion name for a note on the drum channel within the
/// percussion set selected by `bank_lsb` (bank MSB 0x78). A bank LSB that names
/// a known GM2 set returns that set's name where it re-voices the note, falling
/// back to the Standard GM name otherwise. Unknown bank LSBs are treated as the
/// Standard set. Notes outside the GM drum range (35..81) return an empty view.
std::string_view gm2_drum_name(uint8_t bank_lsb, uint8_t note) noexcept;

/// Returns the human-readable name of the GM2 percussion set selected by
/// `bank_lsb` (e.g. "Standard", "Room", "Jazz", "SFX"), or empty for a bank LSB
/// that does not name a GM2 set.
std::string_view gm2_drum_set_name(uint8_t bank_lsb) noexcept;

// ===========================================================================
// Per-destination drum-map override
// ===========================================================================

/// A per-destination drum-note remap: rewrites incoming drum-channel note
/// numbers to the note numbers a particular instrument / sound module expects
/// (e.g. routing a GM "Acoustic Snare" at note 38 to a custom kit's note). It is
/// trivially copyable POD: the CONTROL thread builds it (set_note may allocate
/// nothing — the table is fixed-capacity) and the RT path applies it with
/// map_note() / remap, both allocation-free, lock-free and I/O-free.
///
/// Only entries that are explicitly set remap; any note without an entry passes
/// through unchanged, so an empty override is the identity map. The override is
/// scoped to the drum channel by the caller; map_note() itself does not inspect
/// the channel.
struct DrumMapOverride {
  /// Maximum number of distinct note remaps. set_note() past this fails.
  static constexpr size_t kMaxEntries = 128;

  struct Entry {
    uint8_t from = 0;
    uint8_t to = 0;
  };
  std::array<Entry, kMaxEntries> entries{};
  uint8_t count = 0;

  /// CONTROL thread: map `from` -> `to`. Replaces an existing entry for `from`.
  /// Returns false only when the table is full and `from` is new. `from`/`to`
  /// are masked to 7 bits. A self-map (`from == to`) is stored verbatim.
  bool set_note(uint8_t from, uint8_t to) noexcept {
    const uint8_t f = static_cast<uint8_t>(from & 0x7Fu);
    const uint8_t t = static_cast<uint8_t>(to & 0x7Fu);
    for (uint8_t i = 0; i < count; ++i) {
      if (entries[i].from == f) {
        entries[i].to = t;
        return true;
      }
    }
    if (count >= kMaxEntries) return false;
    entries[count++] = Entry{f, t};
    return true;
  }

  /// Removes the remap for `from`. Returns true if one existed.
  bool clear_note(uint8_t from) noexcept {
    const uint8_t f = static_cast<uint8_t>(from & 0x7Fu);
    for (uint8_t i = 0; i < count; ++i) {
      if (entries[i].from == f) {
        entries[i] = entries[count - 1];
        --count;
        return true;
      }
    }
    return false;
  }

  void clear() noexcept { count = 0; }
  bool empty() const noexcept { return count == 0; }

  /// RT-safe: returns the remapped note for `note`, or `note` itself if it has
  /// no override entry. Allocation-free, lock-free.
  uint8_t map_note(uint8_t note) const noexcept {
    const uint8_t n = static_cast<uint8_t>(note & 0x7Fu);
    for (uint8_t i = 0; i < count; ++i) {
      if (entries[i].from == n) return entries[i].to;
    }
    return n;
  }
};

/// RT-safe: returns a copy of `ump` with its note number remapped through
/// `map` when `ump` is a note-on / note-off / poly-pressure channel-voice
/// message; any other message (and a note with no override) is returned
/// unchanged. The note field is identical for MIDI 1.0 and 2.0, so this works
/// for both protocols. Allocation-free; the caller scopes it to the drum
/// channel. (Defined in program_map.cpp.)
Ump remap_drum_note(const Ump& ump, const DrumMapOverride& map) noexcept;

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
