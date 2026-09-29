#pragma once

/// @file per_note_state.h
/// @brief Per-note pitch state and pitch composition for MIDI 2.0 (M2-104-UM §7.4.5, §7.4.12,
///        §7.4.13, §7.4.15).
///
/// Per-note controllers belong to the note NUMBER, not to a voice: a value sent to a key persists
/// for later notes on that key (§7.4.5). The table therefore holds one row per (channel, note),
/// and each voice carries a PerNoteBinding that reads the live row until a Per-Note Management
/// Detach freezes it onto a snapshot. Note On attribute #3 (Pitch 7.9) applies to one note only,
/// so it is not a row value; the voice captures it at note-on and passes it to the composition.
///
/// Reset All Controllers does not reset per-note controllers or Per-Note Pitch Bend (M2-104-UM
/// B.2), so the table has no channel-wide reset; only a Per-Note Management S=1 or a device reset
/// (clear()) returns a row to unset.
///
/// Everything here is allocation-free and noexcept, safe on the audio thread.

#include <array>
#include <cstdint>

#include "midi/control_value.h"

namespace sonare::midi {

/// RC 0/7 default: 2 semitones in Q7.25. The specification states no default; this matches the
/// MIDI 1.0 channel pitch bend sensitivity default.
inline constexpr Control32 kDefaultPerNoteBendSensitivity = Control32::from_raw(2u << 25);

/// One (channel, note) row, and the frozen copy a detached voice keeps.
struct PerNotePitchSnapshot {
  Bend32 bend = Bend32::center();                 ///< Per-Note Pitch Bend, centered at 0x80000000.
  Control32 pitch_7_25 = Control32::from_raw(0);  ///< RPNC #3 Pitch 7.25 (Q7.25 semitones).
  bool bend_set = false;
  bool pitch_7_25_set = false;
};

/// 16 channels x 128 notes of per-note pitch state.
class PerNotePitchTable {
 public:
  void set_per_note_bend(uint8_t channel, uint8_t note, Bend32 bend) noexcept {
    PerNotePitchSnapshot& r = at(channel, note);
    r.bend = bend;
    r.bend_set = true;
  }

  void set_pitch_7_25(uint8_t channel, uint8_t note, Control32 pitch) noexcept {
    PerNotePitchSnapshot& r = at(channel, note);
    r.pitch_7_25 = pitch;
    r.pitch_7_25_set = true;
  }

  /// Per-Note Management S=1: the row returns to unset.
  void reset(uint8_t channel, uint8_t note) noexcept { at(channel, note) = PerNotePitchSnapshot{}; }

  /// Device reset (power-on / system reset). Not a Reset All Controllers response (B.2).
  void clear() noexcept { rows_ = {}; }

  const PerNotePitchSnapshot& row(uint8_t channel, uint8_t note) const noexcept {
    return rows_[index(channel, note)];
  }

 private:
  static size_t index(uint8_t channel, uint8_t note) noexcept {
    return (static_cast<size_t>(channel & 0x0Fu) << 7) | (note & 0x7Fu);
  }
  PerNotePitchSnapshot& at(uint8_t channel, uint8_t note) noexcept {
    return rows_[index(channel, note)];
  }

  std::array<PerNotePitchSnapshot, 16 * 128> rows_{};
};

/// A voice's link to its key's row.
struct PerNoteBinding {
  uint8_t channel = 0;
  uint8_t note = 0;
  bool detached = false;
  PerNotePitchSnapshot snapshot;

  /// Note-on: attach to (channel, note), so values already on the key apply (§7.4.5).
  void bind(uint8_t ch, uint8_t n) noexcept {
    channel = static_cast<uint8_t>(ch & 0x0Fu);
    note = static_cast<uint8_t>(n & 0x7Fu);
    detached = false;
    snapshot = PerNotePitchSnapshot{};
  }

  /// Per-Note Management D=1: keep the row's current values for the rest of the note.
  void detach(const PerNotePitchTable& table) noexcept {
    if (detached) return;
    snapshot = table.row(channel, note);
    detached = true;
  }

  /// The live row while attached, the snapshot once detached.
  const PerNotePitchSnapshot& pitch_inputs(const PerNotePitchTable& table) const noexcept {
    return detached ? snapshot : table.row(channel, note);
  }
};

/// Per-Note Management for one (channel, note) (§7.4.5). Detach runs first, then Reset, so with
/// D+S the sounding voices keep their values and only future notes see the reset row. D=0 S=0
/// does nothing.
///
/// [first, last) iterates the synth's voices; `binding_of(voice)` returns the voice's
/// PerNoteBinding*, or nullptr for an idle voice. Every active voice bound to the key is
/// detached, releasing ones included (the specification covers "previous notes" too).
template <typename It, typename BindingOf>
void apply_per_note_management(PerNotePitchTable& table, uint8_t channel, uint8_t note, bool detach,
                               bool reset, It first, It last, BindingOf&& binding_of) noexcept {
  const auto ch = static_cast<uint8_t>(channel & 0x0Fu);
  const auto n = static_cast<uint8_t>(note & 0x7Fu);
  if (detach) {
    for (It it = first; it != last; ++it) {
      PerNoteBinding* b = binding_of(*it);
      if (b != nullptr && b->channel == ch && b->note == n) b->detach(table);
    }
  }
  if (reset) table.reset(ch, n);
}

/// Per-Note Pitch Bend in units of the sensitivity: 0x00000000 -> -1, 0x80000000 -> 0,
/// 0xFFFFFFFF -> +1, linear on each side (§7.4.13.1 gives an equal range up and down).
double per_note_bend_units(Bend32 bend) noexcept;

/// Channel pitch bend in cents, computed exactly as NativeSynth and Sf2Player do from a 14-bit
/// value: `(pb - 8192) / 8192 * range_cents` in float, with pb read through Bend32::f14(). For
/// Bend32::from14(v) the result is bit-identical to that expression on v.
float channel_bend_cents(Bend32 bend, float range_cents) noexcept;

/// Note On attribute #3 Pitch 7.9 in semitones.
inline constexpr double attribute_pitch_semitones(uint16_t q7_9) noexcept {
  return static_cast<double>(q7_9) / 512.0;
}

/// Everything that decides one voice's pitch.
struct NotePitchRequest {
  uint8_t note = 60;
  bool has_attribute_pitch = false;  ///< Note On carried attribute #3 (captured at note-on).
  uint16_t attribute_pitch_q7_9 = 0;
  PerNotePitchSnapshot per_note;  ///< PerNoteBinding::pitch_inputs().
  Control32 per_note_bend_sensitivity = kDefaultPerNoteBendSensitivity;  ///< RC 0/7, Q7.25.
  Bend32 channel_bend = Bend32::center();
  float channel_bend_range_cents = 200.0f;
  double coarse_tune_semitones = 0.0;  ///< RPN 0/2.
  double fine_tune_semitones = 0.0;    ///< RPN 0/1.
  double mpe_bend_semitones = 0.0;     ///< MPE member bend, already scaled by the caller.
};

/// Result of compose_note_pitch().
///
/// When `absolute` is true the base came from Pitch 7.9 or Pitch 7.25, which override tuning
/// tables (§7.4.15.2), so a synth does not add GS scale tuning to it; a sample player picks its
/// zone by floor(pitch_semitones) rather than by the note number (§7.4.15 note).
struct ComposedPitch {
  double pitch_semitones = 0.0;  ///< Full pitch as a (fractional) MIDI note number.
  bool absolute = false;
  /// The per-note share: (base - note) + per-note bend. Exactly 0.0 when no per-note input is
  /// present, so a synth that keeps its own float path for the channel terms and adds only this
  /// renders MIDI 1.0 input bit-identically.
  double per_note_semitones = 0.0;
};

/// Pitch of one note (§7.4.15). Base: attribute 7.9 if present, else RPNC #3 7.25 if set, else
/// the note number. Added to it: coarse and fine tune, channel bend x range, per-note bend x
/// RC 0/7 sensitivity, and the MPE member bend.
ComposedPitch compose_note_pitch(const NotePitchRequest& req) noexcept;

}  // namespace sonare::midi
